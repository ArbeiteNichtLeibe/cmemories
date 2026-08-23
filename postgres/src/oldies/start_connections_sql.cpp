// start_connections_sql.cpp
#include "start_connections_sql.hpp"
#include <cstdio>
#include <cstring>

SqlConnectionManager& SqlConnectionManager::getInstance() {
    static SqlConnectionManager instance;
    return instance;
}

bool SqlConnectionManager::initialize(memorymanager::MemoryManagerThread* mm,
                                      const char* socketPath,
                                      const char* user,
                                      const char* password,
                                      const char* dbname,
                                      int poolSize,
                                      char* outError,
                                      size_t errSize) {
    if (initialized_) {
        if (outError) snprintf(outError, errSize, "Already initialized");
        return false;
    }
    if (poolSize < 1 || poolSize > 20) {
        if (outError) snprintf(outError, errSize, "poolSize must be between 1 and 20");
        return false;
    }

    connections_.reserve(poolSize);
    char errBuf[256];

    for (int i = 0; i < poolSize; ++i) {
        auto reader = std::make_unique<SocketReader>(mm);
        if (!reader->connect(socketPath, errBuf, sizeof(errBuf))) {
            if (outError) snprintf(outError, errSize, "Connect failed for connection %d: %s", i, errBuf);
            shutdown();
            return false;
        }
        if (!reader->start(errBuf, sizeof(errBuf))) {
            if (outError) snprintf(outError, errSize, "Start failed for connection %d: %s", i, errBuf);
            shutdown();
            return false;
        }

        auto conn = std::make_unique<PostgresConnection>(*reader, i);
        if (!conn->authenticate(user, password, dbname, errBuf, sizeof(errBuf))) {
            if (outError) snprintf(outError, errSize, "Authentication failed for connection %d: %s", i, errBuf);
            shutdown();
            return false;
        }

        connections_.push_back({std::move(reader), std::move(conn), true});
        fprintf(stderr, "✅ Connection %d authenticated and ready.\n", i);
    }

    initialized_ = true;
    fprintf(stderr, "✅ PostgreSQL connection pool initialized with %d connections.\n", poolSize);
    return true;
}

PostgresConnection* SqlConnectionManager::getConnection(int id) const {
    if (!initialized_ || id < 0 || id >= static_cast<int>(connections_.size()))
        return nullptr;
    return connections_[id].connection.get();
}

int SqlConnectionManager::activeConnections() const {
    if (!initialized_) return 0;
    int count = 0;
    for (const auto& entry : connections_) {
        if (entry.active && entry.connection && entry.connection->isReady())
            ++count;
    }
    return count;
}

void SqlConnectionManager::shutdown() {
    initialized_ = false;
    for (auto it = connections_.rbegin(); it != connections_.rend(); ++it) {
        it->connection.reset();
        it->reader.reset();
        it->active = false;
    }
    connections_.clear();
    fprintf(stderr, "🔌 PostgreSQL connection pool shut down.\n");
}