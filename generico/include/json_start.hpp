#ifndef JSON_START_HPP
#define JSON_START_HPP

#include <cstddef>  // size_t

// Forward declarations das classes usadas (mas as diretrizes proíbem forward declarations,
// então devemos incluir os headers completos – faremos isso abaixo)
#include "../../memorymanager/include/memory_manager_thread.hpp"   // MemoryManagerThread
#include "../../fakeredis/include/fake_redis.hpp"                 // FakeRedis

namespace JSONStart {

/**
 * Inicializa todos os subsistemas:
 * - FakeRedis
 * - JsonGenerator
 * - HttpServer
 * - ExternalBufferSocketManager (substitui o pool PostgreSQL)
 * 
 * @param mm         Gerenciador de memória (arena allocator)
 * @param redis      Instância do FakeRedis (já deve estar alocada)
 * @param runTests   (ignorado – sempre executa testes internos)
 * @param outError   Buffer para mensagem de erro (se falhar)
 * @param errSize    Tamanho do buffer de erro
 * @return true em sucesso, false em falha (com erro em outError)
 */
bool init(memorymanager::MemoryManagerThread* mm,
          FakeRedis::FakeRedis* redis,
          bool runTests,
          char* outError,
          size_t errSize);

/**
 * Desliga todos os subsistemas e libera toda a memória alocada via mm->allocate()
 * (ordem inversa da inicialização).
 */
void shutdown();

} // namespace JSONStart

#endif // JSON_START_HPP