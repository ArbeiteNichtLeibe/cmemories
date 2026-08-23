// socketraw_common.hpp
// Definições compartilhadas entre listener e sender (C++ puro).

#ifndef SOCKETRAW_COMMON_HPP
#define SOCKETRAW_COMMON_HPP

#include <cstring>
#include <string>
#include <libpq-fe.h>
#include <unistd.h>
#include <poll.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <stop_token>

// Inclusão do config (completo)
#include "../../lerconfig/include/config.hpp"

// Constantes
constexpr int MAX_CONNECTIONS = 10;
constexpr size_t BUFFER_SIZE = 15 * 1024 * 1024;        // 15 MB (usado apenas no listener)
constexpr int TOKEN = 0x534F434B;

// Estrutura de configuração (lida do arquivo)
struct ConfigData {
    char pg_socket_dir[256];
    char pg_user[64];
    char pg_dbname[64];
    char pg_port[16];
    int keepalive_seconds;
    int reconnect_delay_ms;
};

// Funções auxiliares (C++ puro, sem extern "C")
bool loadConfig(ConfigData& cfg, std::string& err);
PGconn* connectPg(const ConfigData& cfg, std::string& err);
PGconn* reconnectPg(PGconn* oldConn, const ConfigData& cfg, std::string& err);

#endif // SOCKETRAW_COMMON_HPP