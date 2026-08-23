// postgres_pool.hpp
// ATENÇÃO: Este header não possui include guards. Inclua-o apenas uma vez.

#include "postgres_session.hpp"
#include "../../memorymanager/include/memory_manager_thread.hpp"
#include <cstdint>
#include <atomic>

class PostgresPool {
public:
    PostgresPool();
    ~PostgresPool();

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
    struct Connection {
        uint64_t threadId;
        SocketReader* reader;
        PostgresSession* session;
        bool active;
    };

    bool createConnection(int id, char* outError, size_t errSize);
    void destroyConnection(int id);

    static constexpr int MAX_POOL_SIZE = 10;

    Connection connections_[MAX_POOL_SIZE];
    int poolSize_;
    memorymanager::MemoryManagerThread* mm_;

    char host_[64];
    int port_;
    char dbname_[64];
    char user_[64];
    char password_[64];

    std::atomic<bool> initialized_;
};