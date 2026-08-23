// postgres_pool.cpp
#include "postgres_pool.hpp"
#include <cstring>
#include <chrono>
#include <thread>

PostgresPool::PostgresPool(memorymanager::MemoryManagerThread* mm)
    : mm_(mm), initialized_(false) {}

PostgresPool::~PostgresPool() {
    shutdown();
}

bool PostgresPool::initialize(const char* socketPath, const char* user,
                              const char* password, const char* dbname,
                              int poolSize,
                              char* outError, size_t errSize) {
    if (initialized_) {
        if (outError) snprintf(outError, errSize, "Already initialized");
        return false;
    }
    if (poolSize < 1 || poolSize > 10) {
        if (outError) snprintf(outError, errSize, "poolSize must be 1-10");
        return false;
    }

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < poolSize; ++i) {
        auto sess = std::make_unique<PostgresSession>(mm_);
        char err[256];
        if (!sess->connect(socketPath, user, password, dbname, err, sizeof(err))) {
            if (outError) snprintf(outError, errSize, "Failed session %d: %s", i, err);
            // Fecha as já criadas
            for (auto& s : sessions_) s->close();
            sessions_.clear();
            inUse_.clear();
            return false;
        }
        sessions_.push_back(std::move(sess));
        inUse_.push_back(false);

        // Timeout total de 10 segundos
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(10)) {
            if (outError) snprintf(outError, errSize, "Pool initialization timed out (10s)");
            // Fecha as já criadas
            for (auto& s : sessions_) s->close();
            sessions_.clear();
            inUse_.clear();
            return false;
        }
    }

    initialized_ = true;
    return true;
}

PostgresSession* PostgresPool::getConnection() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return nullptr;
    for (size_t i = 0; i < sessions_.size(); ++i) {
        if (!inUse_[i] && sessions_[i]->isReady()) {
            inUse_[i] = true;
            return sessions_[i].get();
        }
    }
    return nullptr;
}

void PostgresPool::releaseConnection(PostgresSession* session) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return;
    for (size_t i = 0; i < sessions_.size(); ++i) {
        if (sessions_[i].get() == session) {
            inUse_[i] = false;
            return;
        }
    }
}

void PostgresPool::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    for (auto& s : sessions_) {
        s->close();
    }
    sessions_.clear();
    inUse_.clear();
}