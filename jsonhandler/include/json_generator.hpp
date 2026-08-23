#pragma once

#include "../../memorymanager/include/memory_manager_thread.hpp"
#include "../../fakeredis/include/fake_redis.hpp"

#include <atomic>
#include <mutex>
#include <cstddef>

namespace JSONWorker {

class JsonGenerator {
public:
    static JsonGenerator* getInstance();

    void* getTemplateMemory() const { return m_templateMemory; }
    size_t getTemplateMemorySize() const { return m_templateMemorySize; }

    // Inicializa: armazena dependências, aloca 256 MB, chama start_template
    bool init(memorymanager::MemoryManagerThread* mm,
              FakeRedis::FakeRedis* redis,
              char* errBuf,
              size_t errSize);

    static void destroyInstance();
bool addTemplate(const char* nome, const char* conteudo, char* outError, size_t errSize);

private:
    JsonGenerator() = default;
    ~JsonGenerator() = default;

    JsonGenerator(const JsonGenerator&) = delete;
    JsonGenerator& operator=(const JsonGenerator&) = delete;

    memorymanager::MemoryManagerThread* m_mm = nullptr;
    FakeRedis::FakeRedis* m_redis = nullptr;
    bool m_initialized = false;

    void* m_templateMemory = nullptr;
    size_t m_templateMemorySize = 0;

    static JsonGenerator* s_instance;
    static std::atomic<bool> s_created;
    static std::mutex s_mutex;
};

} // namespace JSONWorker