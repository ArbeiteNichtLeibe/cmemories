// postgres_pool_manager.hpp
// ATENÇÃO: Este header não possui include guards. Inclua-o apenas uma vez.

#include "postgres_pool.hpp"
#include <cstdint>

class PostgresPoolManager {
public:
    static PostgresPoolManager& getInstance();

    bool initialize(memorymanager::MemoryManagerThread* mm,
                    const char* host, int port,
                    const char* dbname, const char* user, const char* password,
                    int poolSize = 10,
                    char* outError = nullptr, size_t errSize = 0);

    void shutdown();

    PostgresSession* getConnection(int id) const;
    bool isHealthy(int id) const;
    bool reconnect(int id, char* outError = nullptr, size_t errSize = 0);
    int activeConnections() const;

private:
    PostgresPoolManager() = default;
    ~PostgresPoolManager() = default;

    PostgresPool pool_;
    bool initialized_ = false;
};