// postgres_pool_manager.cpp
#include "postgres_pool_manager.hpp"
#include <cstring>

PostgresPoolManager& PostgresPoolManager::getInstance() {
    static PostgresPoolManager instance;
    return instance;
}

bool PostgresPoolManager::initialize(memorymanager::MemoryManagerThread* mm,
                                     const char* host, int port,
                                     const char* dbname, const char* user,
                                     const char* password,
                                     int poolSize,
                                     char* outError, size_t errSize) {
    if (initialized_) {
        if (outError) snprintf(outError, errSize, "Already initialized");
        return false;
    }

    if (!pool_.initialize(mm, host, port, dbname, user, password, poolSize,
                          outError, errSize)) {
        return false;
    }

    initialized_ = true;
    return true;
}

void PostgresPoolManager::shutdown() {
    if (initialized_) {
        pool_.shutdown();
        initialized_ = false;
    }
}

PostgresSession* PostgresPoolManager::getConnection(int id) const {
    return initialized_ ? pool_.getConnection(id) : nullptr;
}

bool PostgresPoolManager::isHealthy(int id) const {
    return initialized_ && pool_.isHealthy(id);
}

bool PostgresPoolManager::reconnect(int id, char* outError, size_t errSize) {
    return initialized_ ? pool_.reconnect(id, outError, errSize) : false;
}

int PostgresPoolManager::activeConnections() const {
    return initialized_ ? pool_.activeConnections() : 0;
}