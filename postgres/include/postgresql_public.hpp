#ifndef POSTGRESQL_PUBLIC_HPP
#define POSTGRESQL_PUBLIC_HPP

#include <cstddef>
#include <cstdint>
#include "../../memorymanager/include/memory_manager_v2.hpp"   // <-- ADICIONADO

// ============================================================
// API PÚBLICA DO SERVIÇO POSTGRESQL (C++ puro)
// ============================================================
// Todos os ponteiros para o serviço são opacos (void*).
// Nenhuma dependência do libpq é exposta neste header.

// ============================================================
// INICIALIZAÇÃO / FINALIZAÇÃO
// ============================================================

// Inicializa o serviço PostgreSQL com o gerenciador de memória,
// caminho do socket, número máximo de conexões (listeners) e
// retorna ponteiro opaco ou NULL em caso de erro.
void* postgresql_service_init(MemoryManagerV2* memoryManager,   // <-- MUDOU: void* → MemoryManagerV2*
                              const char* pgSocketPath,
                              int maxConnections,
                              char* outError,
                              size_t errSize);

// Finaliza o serviço e libera todos os recursos.
void postgresql_service_shutdown(void* service);

// ============================================================
// LISTENERS – cada listener é uma conexão bloqueante e independente
// ============================================================

// Ativa um listener (conexão) e retorna o ID (0..max-1) ou -1 em erro.
int  postgresql_listener_bring_up(void* service, char* outError, size_t errSize);

// Desativa um listener pelo ID.
bool postgresql_listener_bring_down(void* service, int id, char* outError, size_t errSize);

// Obtém o ponteiro para o buffer circular associado ao listener (opcional).
void* postgresql_listener_get_buffer(void* service, int id);

// Retorna quantos bytes estão prontos no buffer circular (não usado com bloqueio).
size_t postgresql_listener_bytes_ready(void* service, int id);

// Consome 'amount' bytes do buffer circular (não usado com bloqueio).
bool postgresql_listener_consume(void* service, int id, size_t amount, char* outError, size_t errSize);

// Lê dados do buffer circular para 'dest' (não usado com bloqueio).
size_t postgresql_listener_read(void* service, int id, void* dest, size_t maxLen, char* outError, size_t errSize);

// ============================================================
// EXECUÇÃO DE COMANDOS EM LISTENER ESPECÍFICO
// ============================================================

// Executa um comando SQL no listener indicado (conexão bloqueante).
// O resultado (se houver) é colocado em outResult no formato textual (colunas separadas por TAB,
// linhas por \n). Retorna true em caso de sucesso, false com mensagem de erro em outError.
bool postgresql_listener_execute(void* service,
                                 int id,
                                 const char* sql,
                                 char* outResult,
                                 size_t resultSize,
                                 char* outError,
                                 size_t errSize);

// ============================================================
// SENDER (conexão extra bloqueante, opcional)
// ============================================================

// Executa um comando SQL no sender (conexão global).
// Funciona de forma idêntica à listener_execute, mas usa uma conexão dedicada.
bool postgresql_sender_execute(void* service,
                               const char* sql,
                               char* outResult,
                               size_t resultSize,
                               char* outError,
                               size_t errSize);

// ============================================================
// CONSULTAS DE STATUS E INFORMAÇÕES
// ============================================================

// Número máximo de conexões (listeners) configurado.
int  postgresql_get_max_connections(void* service);

// Número de listeners ativos no momento.
int  postgresql_get_active_listeners(void* service);

// Preenche buffer com o caminho do socket PostgreSQL.
void postgresql_get_pg_socket_path(void* service, char* buffer, size_t size);

// Preenche buffer com o caminho dos logs.
void postgresql_get_logs_path(void* service, char* buffer, size_t size);

// Obtém o valor de keepalive (em segundos) usado nas conexões.
int  postgresql_get_keepalive_seconds(void* service);

// Obtém o delay de reconexão (em milissegundos).
int  postgresql_get_reconnect_delay_ms(void* service);

// Preenche um array com os IDs dos listeners ativos. Retorna o número preenchido.
int  postgresql_get_active_listener_ids(void* service, int* ids, int maxIds);

#endif // POSTGRESQL_PUBLIC_HPP