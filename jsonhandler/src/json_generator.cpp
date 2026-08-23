#include "../../jsonhandler/include/json_generator.hpp"
#include "../../uteis/include/uteis.hpp"
#include "../include/config_template.hpp"   // start_template

#include <cstdio>
#include <pthread.h>

namespace JSONWorker {

// Definições estáticas do singleton
JsonGenerator* JsonGenerator::s_instance = nullptr;
std::atomic<bool> JsonGenerator::s_created{false};
std::mutex JsonGenerator::s_mutex;

JsonGenerator* JsonGenerator::getInstance() {
    if (!s_created.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_created.load(std::memory_order_relaxed)) {
            static JsonGenerator instance;
            s_instance = &instance;
            s_created.store(true, std::memory_order_release);
        }
    }
    return s_instance;
}
bool JsonGenerator::addTemplate(const char* nome, const char* conteudo,
                                char* outError, size_t errSize) {
    if (!m_initialized || !m_templateMemory) {
        Utils::safeCopyString(outError, errSize, "JsonGenerator não inicializado ou memória indisponível");
        return false;
    }
    return gravar_template(m_templateMemory, m_templateMemorySize,
                           nome, conteudo, outError, errSize);
}


bool JsonGenerator::init(memorymanager::MemoryManagerThread* mm,
                         FakeRedis::FakeRedis* redis,
                         char* errBuf,
                         size_t errSize) {
    if (!mm || !redis) {
        Utils::safeCopyString(errBuf, errSize, "Ponteiros nulos fornecidos para JsonGenerator::init");
        return false;
    }

    if (m_initialized) {
        Utils::safeCopyString(errBuf, errSize, "JsonGenerator já foi inicializado");
        return false;
    }

    m_mm = mm;
    m_redis = redis;

    // Alocar 256 MB da arena (256 blocos de 1 MB)
    const uint32_t MEGABYTES = 256;
    uint64_t thread_id = static_cast<uint64_t>(pthread_self());
    uint32_t start_block = 0;
    void* start_addr = nullptr;
    void* end_addr = nullptr;

    if (!m_mm->allocate(thread_id, MEGABYTES, start_block, start_addr, end_addr, errBuf)) {
        return false; // errBuf já preenchido por allocate
    }

    // Inicializar templates padrão via config_template
    if (!start_template(start_addr, MEGABYTES * 1024ULL * 1024ULL, errBuf, errSize)) {
        m_mm->free(thread_id, errBuf); // liberar em caso de falha
        return false;
    }

    m_templateMemory = start_addr;
    m_templateMemorySize = MEGABYTES * 1024ULL * 1024ULL;
    m_initialized = true;
    return true;
}

void JsonGenerator::destroyInstance() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_instance) {
        if (s_instance->m_templateMemory && s_instance->m_mm) {
            uint64_t thread_id = static_cast<uint64_t>(pthread_self());
            char errBuf[256];
            s_instance->m_mm->free(thread_id, errBuf);
            s_instance->m_templateMemory = nullptr;
        }
        s_instance->m_mm = nullptr;
        s_instance->m_redis = nullptr;
        s_instance->m_initialized = false;
        s_instance = nullptr;
        s_created.store(false, std::memory_order_release);
    }
}

} // namespace JSONWorker