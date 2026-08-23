// jsonhandler/include/json_worker_instance.hpp
#pragma once

#include "../../memorymanager/include/memory_manager_thread.hpp"
#include "../../fakeredis/include/fake_redis.hpp"
#include <cstdint>
#include <cstddef>

namespace JSONWorker {

/**
 * @brief Processa uma requisição JSON armazenada no Redis.
 * @param requestId ID da chave Redis que contém a requisição (JSON).
 * @param mm Ponteiro para o MemoryManagerThread (já inicializado).
 * @param redis Ponteiro para o FakeRedis (já inicializado).
 * @param outResultId [out] Recebe o ID da nova chave onde o JSON final foi armazenado.
 * @param outError Buffer para mensagem de erro (se falhar).
 * @param errSize Tamanho do buffer de erro.
 * @return true em caso de sucesso, false com erro em outError.
 */
bool processRequest(int64_t requestId,
                    memorymanager::MemoryManagerThread* mm,
                    FakeRedis::FakeRedis* redis,
                    int64_t* outResultId,
                    char* outError,
                    size_t errSize);

} // namespace JSONWorker