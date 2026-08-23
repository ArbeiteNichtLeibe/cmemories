// postgresql_service.cpp
#include "postgresql_service.hpp"
#include "postgresql_public.hpp"
#include "../../lerconfig/include/config.hpp"
#include <cstring>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <mutex>

// ============================================================
// Função auxiliar para extrair diretório e porta do caminho do socket
// ============================================================
static void splitSocketPath(const char* fullPath, char* outDir, size_t dirSize, char* outPort, size_t portSize) {
    const char* lastSlash = strrchr(fullPath, '/');
    if (lastSlash) {
        size_t dirLen = lastSlash - fullPath;
        snprintf(outDir, dirSize, "%.*s", (int)dirLen, fullPath);

        const char* fileName = lastSlash + 1;
        const char* prefix = ".s.PGSQL.";
        const char* portStart = strstr(fileName, prefix);
        if (portStart) {
            portStart += strlen(prefix);
            snprintf(outPort, portSize, "%s", portStart);
        } else {
            snprintf(outPort, portSize, "5432");
        }
    } else {
        snprintf(outDir, dirSize, "%s", fullPath);
        snprintf(outPort, portSize, "5432");
    }
}

// ============================================================
// Construtor / Destrutor
// ============================================================
PostgreSQLService::PostgreSQLService() {
    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        m_listeners[i].reset();
        m_slotActive[i] = false;
        m_listeners[i].id = i;
    }
    m_config.pgSocketPath[0] = '\0';
    m_config.logsPath[0] = '\0';
    m_config.pgUser[0] = '\0';
    m_config.pgDbname[0] = '\0';
    m_config.pgPort[0] = '\0';
    m_config.pgPassword[0] = '\0';
    m_config.keepaliveSeconds = 30;
    m_config.reconnectDelayMs = 5000;
    m_config.maxConnections = 10;
}

PostgreSQLService::~PostgreSQLService() {
    shutdown();
}

// ============================================================
// Inicialização
// ============================================================
bool PostgreSQLService::initialize(MemoryManagerV2* mm, const char* pgSocketPath,
                                   int maxConnections, char* outError, size_t errSize) {
    if (m_initialized) {
        if (outError) snprintf(outError, errSize, "Serviço já inicializado");
        return true;
    }
    if (!mm) {
        snprintf(outError, errSize, "MemoryManager nulo");
        return false;
    }
    m_mm = mm;

    if (!carregarConfiguracao(pgSocketPath, outError, errSize))
        return false;

    int finalMax = (maxConnections > 0) ? maxConnections : m_config.maxConnections;
    if (finalMax < 1) finalMax = 1;
    if (finalMax > MAX_CONNECTIONS) finalMax = MAX_CONNECTIONS;
    m_config.maxConnections = finalMax;

    // Alocar buffers para cada listener (opcional, se ainda quiser usar buffer circular)
    for (int i = 0; i < finalMax; ++i) {
        char err[256];
        void* buf = m_mm->alocarSlot(BUFFER_SIZE, TOKEN_BASE + i, err, sizeof(err));
        if (!buf) {
            snprintf(outError, errSize, "Falha buffer %d: %s", i, err);
            return false;
        }
        m_listeners[i].buffer = buf;
        m_listeners[i].reset();
        m_slotActive[i] = false;
    }

    // Inicializar sender (conexão extra bloqueante, opcional)
    char errSender[256];
    if (!inicializarSender(errSender, sizeof(errSender))) {
        escreverLog("WARN", errSender);
    }

    // Conectar todos os listeners (bloqueantes)
    for (int i = 0; i < finalMax; ++i) {
        char err[256];
        if (!conectarListener(i, err, sizeof(err))) {
            escreverLog("ERROR", err);
        }
    }

    m_initialized = true;
    char msg[128];
    snprintf(msg, sizeof(msg), "Serviço iniciado com %d listeners (bloqueantes)", finalMax);
    escreverLog("INFO", msg);
    return true;
}

void PostgreSQLService::shutdown() {
    if (!m_initialized) return;

    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        if (m_slotActive[i])
            desconectarListener(i);
    }
    fecharSender();
    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        m_listeners[i].buffer = nullptr;
    }
    m_initialized = false;
    escreverLog("INFO", "Serviço finalizado");
}

// ============================================================
// Configuração (lê db_* do config.conf)
// ============================================================
bool PostgreSQLService::carregarConfiguracao(const char* pgSocketPath, char* outError, size_t errSize) {
    const char* defPath = pgSocketPath ? pgSocketPath : "/var/run/postgresql/.s.PGSQL.5432";
    strncpy(m_config.pgSocketPath, defPath, sizeof(m_config.pgSocketPath)-1);
    m_config.pgSocketPath[sizeof(m_config.pgSocketPath)-1] = '\0';
    strncpy(m_config.pgUser, "postgres", sizeof(m_config.pgUser)-1);
    m_config.pgUser[sizeof(m_config.pgUser)-1] = '\0';
    strncpy(m_config.pgDbname, "postgres", sizeof(m_config.pgDbname)-1);
    m_config.pgDbname[sizeof(m_config.pgDbname)-1] = '\0';
    strncpy(m_config.pgPort, "5432", sizeof(m_config.pgPort)-1);
    m_config.pgPort[sizeof(m_config.pgPort)-1] = '\0';
    m_config.pgPassword[0] = '\0';
    m_config.keepaliveSeconds = 30;
    m_config.reconnectDelayMs = 5000;
    m_config.maxConnections = 10;
    strncpy(m_config.logsPath, "/home/andre/Documentos6tb/uploads/memorandos/logs", sizeof(m_config.logsPath)-1);
    m_config.logsPath[sizeof(m_config.logsPath)-1] = '\0';

    try {
        LerConfig::Config& cfg = LerConfig::Config::getInstance();
        if (cfg.carregar("/etc/memorandos/config.conf")) {
            std::string tmp = cfg.getString("db_user", "postgres");
            strncpy(m_config.pgUser, tmp.c_str(), sizeof(m_config.pgUser)-1);
            m_config.pgUser[sizeof(m_config.pgUser)-1] = '\0';

            tmp = cfg.getString("db_name", "postgres");
            strncpy(m_config.pgDbname, tmp.c_str(), sizeof(m_config.pgDbname)-1);
            m_config.pgDbname[sizeof(m_config.pgDbname)-1] = '\0';

            tmp = cfg.getString("db_port", "5432");
            strncpy(m_config.pgPort, tmp.c_str(), sizeof(m_config.pgPort)-1);
            m_config.pgPort[sizeof(m_config.pgPort)-1] = '\0';

            tmp = cfg.getString("db_password", "");
            strncpy(m_config.pgPassword, tmp.c_str(), sizeof(m_config.pgPassword)-1);
            m_config.pgPassword[sizeof(m_config.pgPassword)-1] = '\0';

            tmp = cfg.getString("logs_path", "/home/andre/Documentos6tb/uploads/memorandos/logs");
            strncpy(m_config.logsPath, tmp.c_str(), sizeof(m_config.logsPath)-1);
            m_config.logsPath[sizeof(m_config.logsPath)-1] = '\0';

            m_config.keepaliveSeconds = cfg.getInt("keepalive_seconds", 30);
            m_config.reconnectDelayMs = cfg.getInt("reconnect_delay_ms", 5000);
            int max = cfg.getInt("max_connections", 10);
            if (max >= 1 && max <= MAX_CONNECTIONS) m_config.maxConnections = max;
        } else {
            if (outError) snprintf(outError, errSize, "Config não encontrado, usando padrões");
        }
    } catch (...) {
        if (outError) snprintf(outError, errSize, "Erro na config, usando padrões");
    }
    return true;
}

// ============================================================
// Log (sem std::string, sem std::filesystem)
// ============================================================
bool PostgreSQLService::criarDiretorios(const char* path, char* outError, size_t errSize) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = '\0';
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0) {
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                    snprintf(outError, errSize, "mkdir %s: %s", tmp, strerror(errno));
                    return false;
                }
            }
            *p = '/';
        }
    }
    struct stat st;
    if (stat(tmp, &st) != 0) {
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            snprintf(outError, errSize, "mkdir %s: %s", tmp, strerror(errno));
            return false;
        }
    }
    return true;
}

void PostgreSQLService::escreverLog(const char* nivel, const char* mensagem) {
    if (m_config.logsPath[0] == '\0') return;
    char err[256];
    if (!criarDiretorios(m_config.logsPath, err, sizeof(err))) return;

    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char data[16];
    strftime(data, sizeof(data), "%Y%m%d", &tm_buf);
    char logFile[576];
    snprintf(logFile, sizeof(logFile), "%s/postgresql_%s.log", m_config.logsPath, data);

    char hora[16];
    strftime(hora, sizeof(hora), "%H:%M:%S", &tm_buf);
    FILE* f = fopen(logFile, "a");
    if (f) {
        fprintf(f, "[%s] [%s] %s\n", hora, nivel, mensagem);
        fclose(f);
    }
}

// ============================================================
// Conexão libpq (bloqueante – todas as conexões agora são bloqueantes)
// ============================================================
PGconn* PostgreSQLService::conectarPG(const Config& cfg, char* outError, size_t errSize) {
    char socketDir[256];
    char portStr[8];
    splitSocketPath(cfg.pgSocketPath, socketDir, sizeof(socketDir), portStr, sizeof(portStr));

    char conninfo[1024];
    if (cfg.pgPassword[0] != '\0') {
        snprintf(conninfo, sizeof(conninfo),
                 "host=%s port=%s dbname=%s user=%s password=%s connect_timeout=5 keepalives=1 keepalives_idle=%d",
                 socketDir, portStr, cfg.pgDbname, cfg.pgUser, cfg.pgPassword, cfg.keepaliveSeconds);
    } else {
        snprintf(conninfo, sizeof(conninfo),
                 "host=%s port=%s dbname=%s user=%s connect_timeout=5 keepalives=1 keepalives_idle=%d",
                 socketDir, portStr, cfg.pgDbname, cfg.pgUser, cfg.keepaliveSeconds);
    }
    PGconn* conn = PQconnectdb(conninfo);
    if (PQstatus(conn) != CONNECTION_OK) {
        snprintf(outError, errSize, "PG: %s", PQerrorMessage(conn));
        PQfinish(conn);
        return nullptr;
    }
    // Todas as conexões são bloqueantes – não chamamos PQsetnonblocking
    return conn;
}

PGconn* PostgreSQLService::reconectarPG(PGconn* oldConn, const Config& cfg, char* outError, size_t errSize) {
    if (oldConn) PQfinish(oldConn);
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.reconnectDelayMs));
    return conectarPG(cfg, outError, errSize);
}

// ============================================================
// Sender (opcional – mantido para compatibilidade)
// ============================================================
bool PostgreSQLService::inicializarSender(char* outError, size_t errSize) {
    m_senderConn = conectarPG(m_config, outError, errSize);
    if (!m_senderConn) return false;
    m_senderConnected = true;
    return true;
}

void PostgreSQLService::fecharSender() {
    if (m_senderConn) {
        PQfinish(m_senderConn);
        m_senderConn = nullptr;
    }
    m_senderConnected = false;
}

bool PostgreSQLService::senderExecute(const char* sql, char* outResult, size_t resultSize,
                                      char* outError, size_t errSize) {
    if (!m_initialized) {
        snprintf(outError, errSize, "Serviço não inicializado");
        return false;
    }
    if (!m_senderConnected || !m_senderConn || PQstatus(m_senderConn) != CONNECTION_OK) {
        m_senderConn = reconectarPG(m_senderConn, m_config, outError, errSize);
        if (!m_senderConn) {
            m_senderConnected = false;
            return false;
        }
        m_senderConnected = true;
    }

    PGresult* res = PQexec(m_senderConn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK) {
        snprintf(outError, errSize, "SQL: %s", PQresultErrorMessage(res));
        PQclear(res);
        return false;
    }

    if (outResult && resultSize > 0) {
        outResult[0] = '\0';
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            int nTuples = PQntuples(res);
            int nFields = PQnfields(res);
            size_t pos = 0;
            for (int i = 0; i < nTuples && pos < resultSize-1; ++i) {
                for (int j = 0; j < nFields && pos < resultSize-1; ++j) {
                    const char* val = PQgetvalue(res, i, j);
                    if (val) {
                        size_t len = strlen(val);
                        if (pos + len < resultSize-1) {
                            memcpy(outResult + pos, val, len);
                            pos += len;
                        } else break;
                    }
                    if (j < nFields-1 && pos < resultSize-1) {
                        outResult[pos++] = '\t';
                    }
                }
                if (i < nTuples-1 && pos < resultSize-1) {
                    outResult[pos++] = '\n';
                }
            }
            outResult[pos] = '\0';
        } else {
            snprintf(outResult, resultSize, "Comando executado.");
        }
    }
    PQclear(res);
    return true;
}

// ============================================================
// Listener – agora bloqueante e com capacidade de executar comandos
// ============================================================
bool PostgreSQLService::conectarListener(int id, char* outError, size_t errSize) {
    if (id < 0 || id >= MAX_CONNECTIONS || m_slotActive[id]) {
        snprintf(outError, errSize, "ID inválido ou já ativo");
        return false;
    }
    ListenerConnection& conn = m_listeners[id];
    // Conexão bloqueante (não usamos non-blocking)
    conn.pgConn = conectarPG(m_config, outError, errSize);
    if (!conn.pgConn) return false;

    conn.active.store(true);
    conn.connected.store(true);
    conn.head = 0;
    conn.tail = 0;
    conn.bytesReceived = 0;
    // Removemos o receiverLoop – não há thread escutando
    m_slotActive[id] = true;
    return true;
}

void PostgreSQLService::desconectarListener(int id) {
    if (id < 0 || id >= MAX_CONNECTIONS || !m_slotActive[id]) return;
    ListenerConnection& conn = m_listeners[id];
    conn.active.store(false);
    conn.connected.store(false);
    if (conn.pgConn) {
        PQfinish(conn.pgConn);
        conn.pgConn = nullptr;
    }
    conn.reset();
    m_slotActive[id] = false;
}

// Novo método: executar comando em um listener específico
bool PostgreSQLService::listenerExecute(int id, const char* sql, char* outResult, size_t resultSize,
                                        char* outError, size_t errSize) {
    if (!m_initialized) {
        snprintf(outError, errSize, "Serviço não inicializado");
        return false;
    }
    if (id < 0 || id >= MAX_CONNECTIONS || !m_slotActive[id]) {
        snprintf(outError, errSize, "ID inválido ou inativo");
        return false;
    }

    ListenerConnection& conn = m_listeners[id];
    std::lock_guard<std::mutex> lock(conn.mtx); // protege a conexão para uso exclusivo

    // Verifica se a conexão está OK, senão reconecta
    if (PQstatus(conn.pgConn) != CONNECTION_OK) {
        char reconErr[256];
        conn.pgConn = reconectarPG(conn.pgConn, m_config, reconErr, sizeof(reconErr));
        if (!conn.pgConn) {
            snprintf(outError, errSize, "Falha na reconexão: %s", reconErr);
            conn.connected.store(false);
            return false;
        }
        conn.connected.store(true);
    }

    PGresult* res = PQexec(conn.pgConn, sql);
    if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK) {
        snprintf(outError, errSize, "SQL: %s", PQresultErrorMessage(res));
        PQclear(res);
        return false;
    }

    if (outResult && resultSize > 0) {
        outResult[0] = '\0';
        if (PQresultStatus(res) == PGRES_TUPLES_OK) {
            int nTuples = PQntuples(res);
            int nFields = PQnfields(res);
            size_t pos = 0;
            for (int i = 0; i < nTuples && pos < resultSize-1; ++i) {
                for (int j = 0; j < nFields && pos < resultSize-1; ++j) {
                    const char* val = PQgetvalue(res, i, j);
                    if (val) {
                        size_t len = strlen(val);
                        if (pos + len < resultSize-1) {
                            memcpy(outResult + pos, val, len);
                            pos += len;
                        } else break;
                    }
                    if (j < nFields-1 && pos < resultSize-1) {
                        outResult[pos++] = '\t';
                    }
                }
                if (i < nTuples-1 && pos < resultSize-1) {
                    outResult[pos++] = '\n';
                }
            }
            outResult[pos] = '\0';
        } else {
            snprintf(outResult, resultSize, "Comando executado.");
        }
    }
    PQclear(res);
    return true;
}

// Demais métodos (listenerBringUp, listenerBringDown, getBuffer, etc.)
int PostgreSQLService::listenerBringUp(char* outError, size_t errSize) {
    if (!m_initialized) {
        snprintf(outError, errSize, "Serviço não inicializado");
        return -1;
    }
    for (int i = 0; i < m_config.maxConnections; ++i) {
        if (!m_slotActive[i]) {
            if (conectarListener(i, outError, errSize))
                return i;
            else
                return -1;
        }
    }
    snprintf(outError, errSize, "Nenhum slot livre");
    return -1;
}

bool PostgreSQLService::listenerBringDown(int id, char* outError, size_t errSize) {
    if (!m_initialized) {
        snprintf(outError, errSize, "Serviço não inicializado");
        return false;
    }
    if (id < 0 || id >= MAX_CONNECTIONS || !m_slotActive[id]) {
        snprintf(outError, errSize, "ID inválido ou inativo");
        return false;
    }
    desconectarListener(id);
    return true;
}

void* PostgreSQLService::listenerGetBuffer(int id) {
    if (id < 0 || id >= MAX_CONNECTIONS || !m_slotActive[id]) return nullptr;
    return m_listeners[id].buffer;
}

size_t PostgreSQLService::listenerBytesReady(int id) {
    if (id < 0 || id >= MAX_CONNECTIONS || !m_slotActive[id]) return 0;
    return m_listeners[id].bytesReady();
}

bool PostgreSQLService::listenerConsume(int id, size_t /*amount*/, char* outError, size_t errSize) {
    if (id < 0 || id >= MAX_CONNECTIONS || !m_slotActive[id]) {
        snprintf(outError, errSize, "ID inválido ou inativo");
        return false;
    }
    // Sem receiver, buffer nunca é preenchido – apenas mantemos a API
    (void)id; // se não usar
    return true;
}

size_t PostgreSQLService::listenerRead(int id, void* /*dest*/, size_t /*maxLen*/, char* outError, size_t errSize) {
    if (id < 0 || id >= MAX_CONNECTIONS || !m_slotActive[id]) {
        snprintf(outError, errSize, "ID inválido ou inativo");
        return 0;
    }
    return 0;
}

// ============================================================
// Status
// ============================================================
int PostgreSQLService::getMaxConnections() const { return m_config.maxConnections; }
int PostgreSQLService::getActiveListeners() const {
    int count = 0;
    for (int i = 0; i < MAX_CONNECTIONS; ++i)
        if (m_slotActive[i]) ++count;
    return count;
}
void PostgreSQLService::getPgSocketPath(char* buffer, size_t size) const {
    if (buffer && size > 0) {
        strncpy(buffer, m_config.pgSocketPath, size-1);
        buffer[size-1] = '\0';
    }
}
void PostgreSQLService::getLogsPath(char* buffer, size_t size) const {
    if (buffer && size > 0) {
        strncpy(buffer, m_config.logsPath, size-1);
        buffer[size-1] = '\0';
    }
}
int PostgreSQLService::getKeepaliveSeconds() const { return m_config.keepaliveSeconds; }
int PostgreSQLService::getReconnectDelayMs() const { return m_config.reconnectDelayMs; }
int PostgreSQLService::getActiveListenerIds(int* ids, int maxIds) const {
    int count = 0;
    for (int i = 0; i < MAX_CONNECTIONS && count < maxIds; ++i) {
        if (m_slotActive[i]) {
            ids[count++] = i;
        }
    }
    return count;
}

// ============================================================
// Funções auxiliares (C++ puro, sem extern "C")
// ============================================================
void* postgresql_service_init(MemoryManagerV2* mm, const char* pgSocketPath,
                              int maxConnections, char* outError, size_t errSize) {
    PostgreSQLService* svc = new PostgreSQLService();
    if (!svc->initialize(mm, pgSocketPath, maxConnections, outError, errSize)) {
        delete svc;
        return nullptr;
    }
    return svc;
}

void postgresql_service_shutdown(void* service) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    if (svc) {
        svc->shutdown();
        delete svc;
    }
}

int postgresql_listener_bring_up(void* service, char* outError, size_t errSize) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    if (!svc) { if (outError) snprintf(outError, errSize, "Serviço nulo"); return -1; }
    return svc->listenerBringUp(outError, errSize);
}

bool postgresql_listener_bring_down(void* service, int id, char* outError, size_t errSize) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    if (!svc) { if (outError) snprintf(outError, errSize, "Serviço nulo"); return false; }
    return svc->listenerBringDown(id, outError, errSize);
}

void* postgresql_listener_get_buffer(void* service, int id) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    return svc ? svc->listenerGetBuffer(id) : nullptr;
}

size_t postgresql_listener_bytes_ready(void* service, int id) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    return svc ? svc->listenerBytesReady(id) : 0;
}

bool postgresql_listener_consume(void* service, int id, size_t amount, char* outError, size_t errSize) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    if (!svc) { if (outError) snprintf(outError, errSize, "Serviço nulo"); return false; }
    return svc->listenerConsume(id, amount, outError, errSize);
}

size_t postgresql_listener_read(void* service, int id, void* dest, size_t maxLen, char* outError, size_t errSize) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    if (!svc) { if (outError) snprintf(outError, errSize, "Serviço nulo"); return 0; }
    return svc->listenerRead(id, dest, maxLen, outError, errSize);
}

bool postgresql_sender_execute(void* service, const char* sql, char* outResult, size_t resultSize,
                               char* outError, size_t errSize) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    if (!svc) { if (outError) snprintf(outError, errSize, "Serviço nulo"); return false; }
    return svc->senderExecute(sql, outResult, resultSize, outError, errSize);
}

// NOVA FUNÇÃO: executar comando em um listener específico
bool postgresql_listener_execute(void* service, int id, const char* sql, char* outResult, size_t resultSize,
                                 char* outError, size_t errSize) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    if (!svc) { if (outError) snprintf(outError, errSize, "Serviço nulo"); return false; }
    return svc->listenerExecute(id, sql, outResult, resultSize, outError, errSize);
}

int postgresql_get_max_connections(void* service) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    return svc ? svc->getMaxConnections() : 0;
}

int postgresql_get_active_listeners(void* service) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    return svc ? svc->getActiveListeners() : 0;
}

void postgresql_get_pg_socket_path(void* service, char* buffer, size_t size) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    if (svc) svc->getPgSocketPath(buffer, size);
    else if (buffer && size > 0) buffer[0] = '\0';
}

void postgresql_get_logs_path(void* service, char* buffer, size_t size) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    if (svc) svc->getLogsPath(buffer, size);
    else if (buffer && size > 0) buffer[0] = '\0';
}

int postgresql_get_keepalive_seconds(void* service) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    return svc ? svc->getKeepaliveSeconds() : 0;
}

int postgresql_get_reconnect_delay_ms(void* service) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    return svc ? svc->getReconnectDelayMs() : 0;
}

int postgresql_get_active_listener_ids(void* service, int* ids, int maxIds) {
    PostgreSQLService* svc = static_cast<PostgreSQLService*>(service);
    return svc ? svc->getActiveListenerIds(ids, maxIds) : 0;
}