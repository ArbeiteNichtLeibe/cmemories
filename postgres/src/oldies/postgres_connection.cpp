// postgres_connection.cpp
#include "postgres_connection.hpp"
#include <cstring>
#include <cerrno>
#include <arpa/inet.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <algorithm>

namespace {
    const uint8_t PG_AUTH       = 'R';
    const uint8_t PG_READY      = 'Z';
    const uint8_t PG_ROWDESC    = 'T';
    const uint8_t PG_DATAROW    = 'D';
    const uint8_t PG_CMDCOMPLETE= 'C';
    const uint8_t PG_ERROR      = 'E';
    const uint8_t PG_QUERY      = 'Q';
    const uint8_t PG_PARAMSTATUS= 'S';
    const uint8_t PG_BACKENDKEY = 'K';
}

PostgresConnection::PostgresConnection(SocketReader& reader, int id)
    : reader_(reader),
      connectionId_(id),
      ready_(false),
      hasPassword_(false),
      recvLen_(0),
      recvPos_(0),
      currentResult_(nullptr),
      currentResultSize_(0),
      currentResultLen_(0),
      responseReady_(false),
      authComplete_(false) {
    password_[0] = '\0';
    user_[0] = '\0';
}

PostgresConnection::~PostgresConnection() {}

bool PostgresConnection::authenticate(const char* user, const char* password,
                                      const char* dbname,
                                      char* outError, size_t errSize) {
    if (ready_) {
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Already authenticated", connectionId_);
        return false;
    }

    strncpy(user_, user, sizeof(user_) - 1);
    user_[sizeof(user_) - 1] = '\0';

    if (password) {
        strncpy(password_, password, sizeof(password_) - 1);
        password_[sizeof(password_) - 1] = '\0';
        hasPassword_ = true;
        fprintf(stderr, "[%d] 🔑 Password provided (length %zu)\n", connectionId_, strlen(password_));
    } else {
        hasPassword_ = false;
        fprintf(stderr, "[%d] ⚠️ No password provided\n", connectionId_);
    }

    fprintf(stderr, "[%d] 👤 User: '%s', Database: '%s'\n", connectionId_, user_, dbname);

    if (!sendStartupMessage(user, dbname, outError, errSize))
        return false;

    while (!authComplete_) {
        fprintf(stderr, "[%d] ⏳ Waiting for authentication response...\n", connectionId_);
        if (!waitForResponse(5000)) {
            if (outError && errSize > 0)
                snprintf(outError, errSize, "[%d] Timeout waiting for authentication", connectionId_);
            return false;
        }
        if (!readAndProcess()) {
            if (outError && errSize > 0)
                snprintf(outError, errSize, "[%d] Failed to process auth response", connectionId_);
            return false;
        }
    }

    if (!ready_) {
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Authentication failed (not ready)", connectionId_);
        return false;
    }

    // ============================================================
    // TESTE: executar SELECT 1 para verificar se a conexão está viva
    // ============================================================
    char testResult[64];
    size_t testLen;
    char testErr[256];
    if (!query("SELECT 1;", testResult, sizeof(testResult), testLen, testErr, sizeof(testErr))) {
        ready_ = false;
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Test query (SELECT 1) failed: %s", connectionId_, testErr);
        return false;
    }

    // Verificar se o resultado contém "1"
    bool foundOne = false;
    for (size_t i = 0; i < testLen; ++i) {
        if (testResult[i] == '1') {
            foundOne = true;
            break;
        }
    }
    if (!foundOne) {
        ready_ = false;
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Test query returned unexpected result: '%s'", connectionId_, testResult);
        return false;
    }

    // Descartar quaisquer dados residuais no buffer de entrada
    while (reader_.inData() > 0) {
        uint8_t dummy[1024];
        reader_.readIn(dummy, sizeof(dummy));
    }
    recvLen_ = 0;

    fprintf(stderr, "[%d] ✅ Authentication and test query successful\n", connectionId_);
    return true;
}

bool PostgresConnection::query(const char* sql, char* outResult, size_t outResultSize,
                               size_t& outResultLen, char* outError, size_t errSize) {
    if (!ready_) {
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Not authenticated", connectionId_);
        return false;
    }

    // Resetar flags de controle
    authComplete_ = false;
    responseReady_ = false;
    currentResult_ = outResult;
    currentResultSize_ = outResultSize;
    currentResultLen_ = 0;

    // Enviar a query
    size_t sqlLen = strlen(sql);
    uint32_t msgLen = static_cast<uint32_t>(sqlLen + 4 + 1);
    uint8_t header[5];
    header[0] = PG_QUERY;
    uint32_t netLen = htonl(msgLen);
    memcpy(header + 1, &netLen, 4);

    fprintf(stderr, "[%d] 📤 Sending query: '%s' (len=%zu, msgLen=%u)\n", connectionId_, sql, sqlLen, msgLen);
    fprintf(stderr, "[%d] Header bytes: %02x %02x %02x %02x %02x\n",
            connectionId_, header[0], header[1], header[2], header[3], header[4]);

    size_t sent;
    sent = reader_.writeOut(header, 5);
    fprintf(stderr, "[%d] writeOut header returned %zu (expected 5)\n", connectionId_, sent);
    if (sent != 5) {
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Failed to send query header (sent %zu)", connectionId_, sent);
        return false;
    }

    sent = reader_.writeOut(reinterpret_cast<const uint8_t*>(sql), sqlLen);
    fprintf(stderr, "[%d] writeOut sql returned %zu (expected %zu)\n", connectionId_, sent, sqlLen);
    if (sent != sqlLen) {
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Failed to send query body (sent %zu)", connectionId_, sent);
        return false;
    }

    const uint8_t nullByte = 0;
    sent = reader_.writeOut(&nullByte, 1);
    fprintf(stderr, "[%d] writeOut terminator returned %zu (expected 1)\n", connectionId_, sent);
    if (sent != 1) {
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Failed to send query terminator (sent %zu)", connectionId_, sent);
        return false;
    }

    // Pequena pausa para dar tempo à thread de envio
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Loop até receber ReadyForQuery ou erro
    while (!authComplete_) {
        if (!waitForResponse(10000)) {
            if (outError && errSize > 0)
                snprintf(outError, errSize, "[%d] Timeout waiting for query response", connectionId_);
            return false;
        }
        if (!readAndProcess()) {
            if (outError && errSize > 0)
                snprintf(outError, errSize, "[%d] Failed to process query response", connectionId_);
            return false;
        }
    }

    outResultLen = currentResultLen_;
    fprintf(stderr, "[%d] Query result: '%s' (len=%zu)\n", connectionId_, outResult, outResultLen);
    return true;
}

// ------------------------------------------------------------
// Métodos privados
// ------------------------------------------------------------

bool PostgresConnection::sendStartupMessage(const char* user, const char* dbname,
                                            char* outError, size_t errSize) {
    uint32_t userLen = strlen(user) + 1;
    uint32_t dbLen   = strlen(dbname) + 1;
    uint32_t totalLen = 4 + 4 + 5 + userLen + 9 + dbLen + 1;

    uint8_t buffer[512];
    if (totalLen > sizeof(buffer)) {
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Startup message too large", connectionId_);
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

    if (reader_.writeOut(buffer, pos) < pos) {
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Failed to send startup message", connectionId_);
        return false;
    }
    return true;
}

bool PostgresConnection::sendPasswordMessage(const char* password,
                                             char* outError, size_t errSize) {
    size_t pwdLen = strlen(password);
    uint32_t msgLen = static_cast<uint32_t>(4 + pwdLen + 1);
    uint8_t buffer[512];
    if (msgLen + 1 > sizeof(buffer)) {
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Password too long", connectionId_);
        return false;
    }
    buffer[0] = 'p';
    uint32_t netLen = htonl(msgLen);
    memcpy(buffer + 1, &netLen, 4);
    memcpy(buffer + 5, password, pwdLen);
    buffer[5 + pwdLen] = 0;

    if (reader_.writeOut(buffer, msgLen + 1) < msgLen + 1) {
        if (outError && errSize > 0)
            snprintf(outError, errSize, "[%d] Failed to send password", connectionId_);
        return false;
    }
    fprintf(stderr, "[%d] 📤 Sending password (cleartext)\n", connectionId_);
    return true;
}

bool PostgresConnection::waitForResponse(int timeoutMs) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (responseReady_) return true;
        if (reader_.inData() > 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::milliseconds(timeoutMs))
            return false;
    }
}

bool PostgresConnection::readAndProcess() {
    while (reader_.inData() > 0) {
        size_t avail = reader_.inData();
        size_t space = sizeof(recvBuffer_) - recvLen_;
        size_t toRead = (avail < space) ? avail : space;
        size_t n = reader_.readIn(recvBuffer_ + recvLen_, toRead);
        if (n == 0) break;
        recvLen_ += n;

        while (recvLen_ >= 5) {
            uint32_t msgLen;
            memcpy(&msgLen, recvBuffer_ + 1, 4);
            msgLen = ntohl(msgLen);
            if (recvLen_ < msgLen + 1) break;

            uint8_t type = recvBuffer_[0];
            const uint8_t* msg = recvBuffer_;

            fprintf(stderr, "[%d] 📨 Received message type: 0x%02x (%c) len=%u\n",
                    connectionId_, type, (type >= 32 && type <= 126) ? type : '?', msgLen);

            switch (type) {
                case PG_AUTH: {
                    uint32_t authType;
                    memcpy(&authType, msg + 5, 4);
                    authType = ntohl(authType);
                    fprintf(stderr, "[%d] 🔐 Authentication type: %u\n", connectionId_, authType);
                    if (authType == 0) {
                        ready_ = true;
                        responseReady_ = true;
                        authComplete_ = true;
                        fprintf(stderr, "[%d] ✅ Authentication OK (type 0)\n", connectionId_);
                    } else if (authType == 3) {
                        if (hasPassword_) {
                            sendPasswordMessage(password_, nullptr, 0);
                        } else {
                            ready_ = false;
                            responseReady_ = true;
                            authComplete_ = true;
                            fprintf(stderr, "[%d] ❌ No password provided\n", connectionId_);
                        }
                    } else if (authType == 5) {
                        if (hasPassword_) {
                            sendPasswordMessage(password_, nullptr, 0);
                        } else {
                            ready_ = false;
                            responseReady_ = true;
                            authComplete_ = true;
                            fprintf(stderr, "[%d] ❌ No password provided for MD5\n", connectionId_);
                        }
                    } else {
                        fprintf(stderr, "[%d] ❌ Unsupported auth type: %u\n", connectionId_, authType);
                        ready_ = false;
                        responseReady_ = true;
                        authComplete_ = true;
                    }
                    break;
                }

                case PG_READY:
                    ready_ = true;
                    responseReady_ = true;
                    authComplete_ = true;
                    fprintf(stderr, "[%d] ✅ ReadyForQuery received\n", connectionId_);
                    break;

                case PG_CMDCOMPLETE: {
                    ready_ = true;
                    responseReady_ = true;
                    authComplete_ = true;
                    fprintf(stderr, "[%d] ✅ CommandComplete received\n", connectionId_);
                    break;
                }

                case PG_DATAROW: {
                    const uint8_t* ptr = msg + 5;
                    uint16_t numCols;
                    memcpy(&numCols, ptr, 2);
                    numCols = ntohs(numCols);
                    ptr += 2;

                    fprintf(stderr, "[%d] 📊 DataRow: numCols=%u\n", connectionId_, numCols);

                    if (currentResult_ && currentResultSize_ > 0) {
                        size_t pos = currentResultLen_;
                        for (int i = 0; i < numCols; ++i) {
                            uint32_t colLen;
                            memcpy(&colLen, ptr, 4);
                            colLen = ntohl(colLen);
                            ptr += 4;

                            if (i > 0 && pos < currentResultSize_ - 1) {
                                currentResult_[pos++] = ' ';
                            }

                            if (colLen == 0xFFFFFFFF) { // NULL
                                if (pos + 4 < currentResultSize_) {
                                    memcpy(currentResult_ + pos, "NULL", 4);
                                    pos += 4;
                                }
                            } else {
                                if (pos + colLen < currentResultSize_) {
                                    memcpy(currentResult_ + pos, ptr, colLen);
                                    pos += colLen;
                                    ptr += colLen;
                                } else {
                                    pos = currentResultSize_ - 1;
                                    break;
                                }
                            }
                        }
                        currentResult_[pos] = '\0';
                        currentResultLen_ = pos;
                        fprintf(stderr, "[%d] 📊 Result: '%s'\n", connectionId_, currentResult_);
                    }
                    responseReady_ = true;
                    break;
                }

                case PG_PARAMSTATUS:
                    fprintf(stderr, "[%d] 📨 ParameterStatus (ignored)\n", connectionId_);
                    break;

                case PG_BACKENDKEY:
                    fprintf(stderr, "[%d] 📨 BackendKeyData (ignored)\n", connectionId_);
                    break;

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
                    fprintf(stderr, "[%d] ❌ PostgreSQL error: %s\n", connectionId_, errMsg);
                    ready_ = false;
                    responseReady_ = true;
                    authComplete_ = true;
                    break;
                }

                default:
                    fprintf(stderr, "[%d] 📨 Unhandled message type: 0x%02x\n", connectionId_, type);
                    break;
            }

            size_t consumed = msgLen + 1;
            if (recvLen_ > consumed) {
                memmove(recvBuffer_, recvBuffer_ + consumed, recvLen_ - consumed);
                recvLen_ -= consumed;
            } else {
                recvLen_ = 0;
            }

            if (authComplete_) {
                return true;
            }
        }
    }
    return true;
}

SocketReader& PostgresConnection::getReader() {
    return reader_;
}