#ifndef INJECTPOSTGRESQL_HPP
#define INJECTPOSTGRESQL_HPP

#include <cstddef>
#include "../../memorymanager/include/memory_manager_v2.hpp"

// Todas as funções são C++ puras, sem extern "C"
void* injec_postgresql_init(MemoryManagerV2* memoryManager,
                            const char* pgSocketPath,
                            int maxConnections,
                            char* outError,
                            size_t errSize);

void injec_postgresql_shutdown(void* service);

int injec_postgresql_get_max_connections();
void injec_postgresql_get_pg_socket_path(char* buffer, size_t size);
void injec_postgresql_get_logs_path(char* buffer, size_t size);
int injec_postgresql_get_keepalive_seconds();
int injec_postgresql_get_reconnect_delay_ms();
int injec_postgresql_get_conexoes_abertas(int* ids, int maxIds);

#endif