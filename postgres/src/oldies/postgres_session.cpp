// postgres_session.cpp
#include "postgres_session.hpp"
#include <cstring>
#include <cerrno>
#include <arpa/inet.h>
#include <chrono>
#include <thread>
#include <cstdio>

namespace {
    const uint8_t PG_QUERY      = 'Q';
    const uint8_t PG_TERMINATE  = 'X';
    const uint8_t PG_AUTH       = 'R';
    const uint8_t PG_READY      = 'Z';
    const uint8_t PG_ROWDESC    = 'T';
    const uint8_t PG_DATAROW    = 'D';
    const uint8_t PG_CMDCOMPLETE= 'C';
    const uint8_t PG_ERROR      = 'E';
    const uint8_t PG_NOTICE     = 'N';
    const uint8_t PG_PARAMSTATUS= 'S';
    const uint8_t PG_BACKENDKEY = 'K';
}

PostgresSession::PostgresSession(memorymanager::MemoryManagerThread* mm)
    : reader_(mm),
      state_(State::Disconnected),
      readLen_(0), readPos_(0),
      hasPassword_(false),
      currentResult_(nullptr),
      currentResultSize_(0),
      currentResultLen_(0),
      responseReady_(false) {
    password_[0] = '\0';
}

PostgresSession::~PostgresSession() {
    close();
}

bool PostgresSession::connect(const char* socketPath,
                              const char* user,
                              const char* password,
                              const char* dbname,
                              char* outError, size_t errSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Disconnected) {
        snprintf(outError, errSize, "Already connected or in use");
        return false;
    }

    if (!reader_.connect(socketPath, outError, errSize))
        return false;

    if (!reader_.start(outError, errSize))
        return false;

    if (password) {
        strncpy(password_, password, sizeof(password_) - 1);
        password_[sizeof(password_) - 1] = '\0';
        hasPassword_ = true;
    } else {
        password_[0] = '\0';
        hasPassword_ = false;
    }

    state_ = State::Startup;

    if (!sendStartupMessage(user, dbname, outError, errSize)) {
        close();
        return false;
    }

    // Timeout de 5 segundos para autenticação
    if (!waitForResponse(5000)) {
        snprintf(outError, errSize, "Timeout waiting for authentication response");
        close();
        return false;
    }

    if (!processIncoming()) {
        close();
        return false;
    }

    if (state_ != State::Ready) {
        snprintf(outError, errSize, "Authentication failed (state=%d)", static_cast<int>(state_));
        close();
        return false;
    }

    return true;
}

bool PostgresSession::execute(const char* query, size_t queryLen,
                              char* outResult, size_t outResultSize,
                              size_t& outResultLen,
                              char* outError, size_t errSize) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Ready) {
        snprintf(outError, errSize, "Session not ready");
        return false;
    }

    uint32_t msgLen = static_cast<uint32_t>(queryLen + 4 + 1);
    uint8_t header[5];
    header[0] = PG_QUERY;
    uint32_t netLen = htonl(msgLen);
    memcpy(header + 1, &netLen, 4);

    size_t sent = reader_.writeOut(header, 5);
    if (sent < 5) {
        snprintf(outError, errSize, "Failed to send query header");
        return false;
    }
    sent = reader_.writeOut(reinterpret_cast<const uint8_t*>(query), queryLen);
    if (sent < queryLen) {
        snprintf(outError, errSize, "Failed to send query body");
        return false;
    }
    const uint8_t nullByte = 0;
    sent = reader_.writeOut(&nullByte, 1);
    if (sent < 1) {
        snprintf(outError, errSize, "Failed to send query terminator");
        return false;
    }

    state_ = State::QueryPending;
    currentResult_ = outResult;
    currentResultSize_ = outResultSize;
    currentResultLen_ = 0;
    responseReady_.store(false);

    // Timeout de 10 segundos para consulta
    if (!waitForResponse(10000)) {
        snprintf(outError, errSize, "Timeout waiting for query response");
        state_ = State::Error;
        close();
        return false;
    }

    if (!processIncoming()) {
        close();
        return false;
    }

    if (state_ != State::Ready) {
        snprintf(outError, errSize, "Query failed (state=%d)", static_cast<int>(state_));
        close();
        return false;
    }

    outResultLen = currentResultLen_;
    return true;
}

void PostgresSession::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Disconnected) {
        if (state_ != State::Error) {
            uint8_t terminate[] = { PG_TERMINATE, 0, 0, 0, 4 };
            reader_.writeOut(terminate, sizeof(terminate));
        }
        reader_.stop();
        state_ = State::Disconnected;
        currentResult_ = nullptr;
        currentResultSize_ = 0;
        currentResultLen_ = 0;
        responseReady_.store(false);
        readLen_ = 0;
        readPos_ = 0;
    }
}

// ------------------------------------------------------------
// Métodos privados (corrigidos)
// ------------------------------------------------------------

bool PostgresSession::sendStartupMessage(const char* user, const char* dbname,
                                         char* outError, size_t errSize) {
    uint32_t userLen = strlen(user) + 1;
    uint32_t dbLen   = strlen(dbname) + 1;
    uint32_t totalLen = 4 + 4 + 5 + userLen + 9 + dbLen + 1;

    uint8_t buffer[512];
    if (totalLen > sizeof(buffer)) {
        snprintf(outError, errSize, "Startup message too large");
        return false;
    }

    uint32_t netLen = htonl(totalLen);
    uint32_t netProto = htonl(196608);

    size_t pos = 0;
    memcpy(buffer + pos, &netLen, 4); pos += 4;
    memcpy(buffer + pos, &netProto, 4); pos += 4;

    memcpy(buffer + pos, "user", 5); pos += 5;
    memcpy(buffer + pos, user, userLen); pos += userLen;

    memcpy(buffer + pos, "database", 9); pos += 9;
    memcpy(buffer + pos, dbname, dbLen); pos += dbLen;

    buffer[pos] = 0; pos += 1;

    size_t sent = reader_.writeOut(buffer, pos);
    if (sent < pos) {
        snprintf(outError, errSize, "Failed to send startup message");
        return false;
    }
    return true;
}

bool PostgresSession::sendPasswordMessage(const char* password,
                                          char* outError, size_t errSize) {
    size_t pwdLen = strlen(password);
    uint32_t msgLen = static_cast<uint32_t>(4 + pwdLen + 1);
    uint8_t buffer[512];
    if (msgLen + 1 > sizeof(buffer)) {
        snprintf(outError, errSize, "Password too long");
        return false;
    }
    buffer[0] = 'p';
    uint32_t netLen = htonl(msgLen);
    memcpy(buffer + 1, &netLen, 4);
    memcpy(buffer + 5, password, pwdLen);
    buffer[5 + pwdLen] = 0;

    size_t sent = reader_.writeOut(buffer, msgLen + 1);
    if (sent < msgLen + 1) {
        snprintf(outError, errSize, "Failed to send password");
        return false;
    }
    return true;
}

bool PostgresSession::waitForResponse(int timeoutMs) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (responseReady_.load()) return true;
        if (reader_.inData() > 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::milliseconds(timeoutMs)) return false;
    }
}

bool PostgresSession::readAndProcess() {
    size_t avail = reader_.inData();
    if (avail == 0) return true;

    size_t space = sizeof(readBuffer_) - readLen_;
    size_t toRead = std::min(avail, space);
    size_t n = reader_.readIn(readBuffer_ + readLen_, toRead);
    if (n == 0) return true;
    readLen_ += n;

    size_t consumed = 0;
    while (readLen_ - consumed >= 5) {
        uint32_t msgLen;
        memcpy(&msgLen, readBuffer_ + consumed + 1, 4);
        msgLen = ntohl(msgLen);

        if (readLen_ - consumed < msgLen + 1) {
            break;
        }

        if (!handleMessage(readBuffer_ + consumed, consumed)) {
            return false;
        }
    }

    if (consumed > 0) {
        size_t remaining = readLen_ - consumed;
        if (remaining > 0) {
            memmove(readBuffer_, readBuffer_ + consumed, remaining);
        }
        readLen_ = remaining;
    }
    return true;
}

bool PostgresSession::handleMessage(const uint8_t* msg, size_t& consumed) {
    uint8_t type = msg[0];
    uint32_t msgLen;
    memcpy(&msgLen, msg + 1, 4);
    msgLen = ntohl(msgLen);

    bool processed = false;

    switch (type) {
        case PG_AUTH: {
            uint32_t authType;
            memcpy(&authType, msg + 5, 4);
            authType = ntohl(authType);
            if (authType == 0) {
                state_ = State::Ready;
                processed = true;
            } else if (authType == 3 || authType == 5) {
                if (hasPassword_) {
                    if (!sendPasswordMessage(password_, nullptr, 0)) {
                        state_ = State::Error;
                        processed = false;
                    } else {
                        state_ = State::AuthWaiting;
                        processed = true;
                    }
                } else {
                    state_ = State::Error;
                    processed = false;
                }
            } else {
                fprintf(stderr, "❌ Unsupported auth type: %u (SCRAM?)\n", authType);
                state_ = State::Error;
                processed = false;
            }
            break;
        }
        case PG_READY:
            state_ = State::Ready;
            processed = true;
            break;
        case PG_CMDCOMPLETE: {
            state_ = State::Ready;
            const char* cmdText = reinterpret_cast<const char*>(msg + 5);
            size_t cmdLen = strlen(cmdText);
            if (currentResult_ && currentResultSize_ > 0) {
                size_t toCopy = std::min(cmdLen, currentResultSize_ - 1);
                memcpy(currentResult_, cmdText, toCopy);
                currentResult_[toCopy] = '\0';
                currentResultLen_ = toCopy;
            }
            processed = true;
            break;
        }
        case PG_ROWDESC:
            processed = true;
            break;
        case PG_DATAROW: {
            const uint8_t* data = msg + 5;
            size_t dataLen = msgLen - 4;
            if (currentResult_ && currentResultSize_ > 0) {
                size_t space = currentResultSize_ - currentResultLen_ - 1;
                if (space > 0) {
                    size_t toCopy = std::min(dataLen, space);
                    memcpy(currentResult_ + currentResultLen_, data, toCopy);
                    currentResultLen_ += toCopy;
                    currentResult_[currentResultLen_] = '\0';
                }
            }
            processed = true;
            break;
        }
        case PG_ERROR: {
            const char* errMsg = "unknown error";
            const uint8_t* p = msg + 5;
            while (p < msg + msgLen + 1) {
                if (*p == 'M') {
                    p++;
                    errMsg = reinterpret_cast<const char*>(p);
                    break;
                }
                while (*p) p++;
                p++;
            }
            fprintf(stderr, "❌ PostgreSQL error: %s\n", errMsg);
            state_ = State::Error;
            processed = false;
            break;
        }
        default:
            processed = true;
            break;
    }

    consumed += msgLen + 1;
    if (processed) {
        responseReady_.store(true);
    }
    return processed;
}

bool PostgresSession::processIncoming() {
    while (true) {
        if (reader_.inData() == 0) break;
        if (!readAndProcess()) return false;
        if (state_ == State::Ready || state_ == State::Error) break;
    }
    return true;
}