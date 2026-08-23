#ifndef POSTGRESQL_SERVICE_HPP
#define POSTGRESQL_SERVICE_HPP

#include <atomic>
#include <thread>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <libpq-fe.h>

// Inclui a definição completa de MemoryManagerV2 (sem forward declaration)
#include "../../memorymanager/include/memory_manager_v2.hpp"

#define MAX_CONNECTIONS 10
#define BUFFER_SIZE (64 * 1024)
#define TOKEN_BASE 0x50475300  // "PGS"

class PostgreSQLService {
public:
    PostgreSQLService();
    ~PostgreSQLService();

    bool initialize(MemoryManagerV2* mm, const char* pgSocketPath,
                    int maxConnections, char* outError, size_t errSize);
    void shutdown();

    // --- Sender (opcional) ---
    bool senderExecute(const char* sql, char* outResult, size_t resultSize,
                       char* outError, size_t errSize);

    // --- Listener (bloqueante) ---
    int  listenerBringUp(char* outError, size_t errSize);
    bool listenerBringDown(int id, char* outError, size_t errSize);
    void* listenerGetBuffer(int id);
    size_t listenerBytesReady(int id);
    bool listenerConsume(int id, size_t amount, char* outError, size_t errSize);
    size_t listenerRead(int id, void* dest, size_t maxLen, char* outError, size_t errSize);

    // Executa comando SQL em um listener específico
    bool listenerExecute(int id, const char* sql, char* outResult, size_t resultSize,
                         char* outError, size_t errSize);

    // --- Status ---
    int  getMaxConnections() const;
    int  getActiveListeners() const;
    void getPgSocketPath(char* buffer, size_t size) const;
    void getLogsPath(char* buffer, size_t size) const;
    int  getKeepaliveSeconds() const;
    int  getReconnectDelayMs() const;
    int  getActiveListenerIds(int* ids, int maxIds) const;

private:
    struct Config {
        char pgSocketPath[256];
        char logsPath[256];
        char pgUser[64];
        char pgDbname[64];
        char pgPort[8];
        char pgPassword[64];
        int  keepaliveSeconds;
        int  reconnectDelayMs;
        int  maxConnections;
    };

    struct ListenerConnection {
        int id;
        PGconn* pgConn = nullptr;
        void* buffer = nullptr;
        std::atomic<bool> active{false};
        std::atomic<bool> connected{false};
        std::atomic<size_t> head{0};
        std::atomic<size_t> tail{0};
        std::atomic<size_t> bytesReceived{0};
        std::mutex mtx;

        void reset() {
            active.store(false);
            connected.store(false);
            head.store(0);
            tail.store(0);
            bytesReceived.store(0);
        }

        size_t bytesReady() const {
            size_t h = head.load(std::memory_order_acquire);
            size_t t = tail.load(std::memory_order_acquire);
            return (h >= t) ? (h - t) : (BUFFER_SIZE - t + h);
        }
    };

    MemoryManagerV2* m_mm = nullptr;
    Config m_config;
    bool m_initialized = false;

    PGconn* m_senderConn = nullptr;
    bool m_senderConnected = false;

    ListenerConnection m_listeners[MAX_CONNECTIONS];
    bool m_slotActive[MAX_CONNECTIONS];

    bool carregarConfiguracao(const char* pgSocketPath, char* outError, size_t errSize);
    bool criarDiretorios(const char* path, char* outError, size_t errSize);
    void escreverLog(const char* nivel, const char* mensagem);

    PGconn* conectarPG(const Config& cfg, char* outError, size_t errSize);
    PGconn* reconectarPG(PGconn* oldConn, const Config& cfg, char* outError, size_t errSize);

    bool inicializarSender(char* outError, size_t errSize);
    void fecharSender();

    bool conectarListener(int id, char* outError, size_t errSize);
    void desconectarListener(int id);
};

#endif // POSTGRESQL_SERVICE_HPP