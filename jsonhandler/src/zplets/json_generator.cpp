#include "json_generator.hpp"
#include "../../memorymanager/include/memory_manager_thread.hpp"
#include "../../fakeredis/include/fake_redis.hpp"
#include "config_template.hpp"
#include <cstdio>
#include <chrono>
#include <cstring>

namespace JSONWorker {

// Static buffers (no stack, no heap)
static char g_errBuf[256];
static char g_tmpBuf[4096];
static char g_dummy[128];

JsonGenerator* JsonGenerator::singleton = nullptr;
std::mutex JsonGenerator::instanceMutex;

JsonGenerator* JsonGenerator::getInstance() {
    std::lock_guard<std::mutex> lock(instanceMutex);
    if (!singleton) singleton = new JsonGenerator();
    return singleton;
}

void JsonGenerator::destroyInstance() {
    std::lock_guard<std::mutex> lock(instanceMutex);
    delete singleton;
    singleton = nullptr;
}

JsonGenerator::JsonGenerator()
    : m_mm(nullptr)
    , m_redis(nullptr)
    , m_templateMemory(nullptr)
    , m_jsonMemory(nullptr)
    , m_running(false)
    , m_inicializado(false) {
}

JsonGenerator::~JsonGenerator() {
    shutdown();
}

bool JsonGenerator::init(memorymanager::MemoryManagerThread* mm,
                         FakeRedis::FakeRedis* redis,
                         char* outError, size_t errSize) {
    if (m_inicializado) return true;
    if (!mm || !redis) {
        if (outError) snprintf(outError, errSize, "Null parameters");
        return false;
    }

    m_mm = mm;
    m_redis = redis;

    const uint32_t TEMPLATE_BLOCKS = 256; // 256 MB
    uint32_t startBlock1 = 0;
    void* startAddr1 = nullptr;
    void* endAddr1 = nullptr;
    if (!m_mm->allocate(0x0001, TEMPLATE_BLOCKS, startBlock1, startAddr1, endAddr1, outError)) {
        return false;
    }
    m_templateMemory = startAddr1;

    const uint32_t JSON_BLOCKS = 1024; // 1 GB
    uint32_t startBlock2 = 0;
    void* startAddr2 = nullptr;
    void* endAddr2 = nullptr;
    if (!m_mm->allocate(0x0002, JSON_BLOCKS, startBlock2, startAddr2, endAddr2, outError)) {
        m_mm->free(0x0001, g_dummy);
        return false;
    }
    m_jsonMemory = startAddr2;

    // Memory test
    uint8_t* test1 = static_cast<uint8_t*>(m_templateMemory);
    test1[0] = 0xAA;
    test1[1] = 0xBB;
    if (test1[0] != 0xAA || test1[1] != 0xBB) {
        if (outError) snprintf(outError, errSize, "Memory test failed (template)");
        return false;
    }

    uint8_t* test2 = static_cast<uint8_t*>(m_jsonMemory);
    test2[0] = 0xCC;
    test2[1] = 0xDD;
    if (test2[0] != 0xCC || test2[1] != 0xDD) {
        if (outError) snprintf(outError, errSize, "Memory test failed (json)");
        return false;
    }

    fprintf(stdout, "✅ Memory tests OK.\n");

    // Initialize default templates
    if (!start_template(m_templateMemory, TEMPLATE_BLOCKS * 1024 * 1024, outError, errSize)) {
        m_mm->free(0x0001, g_dummy);
        m_mm->free(0x0002, g_dummy);
        return false;
    }
    fprintf(stdout, "✅ Default templates initialized.\n");

    // Test: consult template
    if (consultar_template(m_templateMemory, TEMPLATE_BLOCKS * 1024 * 1024,
                           "template_01", g_tmpBuf, sizeof(g_tmpBuf),
                           g_errBuf, sizeof(g_errBuf))) {
        fprintf(stdout, "✅ Query template_01: %s\n", g_tmpBuf);
    } else {
        fprintf(stderr, "⚠️ Query failed: %s\n", g_errBuf);
    }

    // Test: write new template
    const char* novoNome = "my_template";
    const char* novoConteudo = "{\"key\":\"value\"}";
    if (gravar_template(m_templateMemory, TEMPLATE_BLOCKS * 1024 * 1024,
                        novoNome, novoConteudo, g_errBuf, sizeof(g_errBuf))) {
        fprintf(stdout, "✅ Template '%s' written successfully.\n", novoNome);
        if (consultar_template(m_templateMemory, TEMPLATE_BLOCKS * 1024 * 1024,
                               novoNome, g_tmpBuf, sizeof(g_tmpBuf),
                               g_errBuf, sizeof(g_errBuf))) {
            fprintf(stdout, "✅ Query new template: %s\n", g_tmpBuf);
        } else {
            fprintf(stderr, "⚠️ Failed to query new template: %s\n", g_errBuf);
        }
    } else {
        fprintf(stderr, "⚠️ Write failed: %s\n", g_errBuf);
    }

    // Test: remove template
    const char* removerNome = "template_02";
    if (remover_template(m_templateMemory, TEMPLATE_BLOCKS * 1024 * 1024,
                         removerNome, g_errBuf, sizeof(g_errBuf))) {
        fprintf(stdout, "✅ Template '%s' removed successfully.\n", removerNome);
        if (!consultar_template(m_templateMemory, TEMPLATE_BLOCKS * 1024 * 1024,
                                removerNome, g_tmpBuf, sizeof(g_tmpBuf),
                                g_errBuf, sizeof(g_errBuf))) {
            fprintf(stdout, "✅ Confirmation: removed template is no longer accessible.\n");
        } else {
            fprintf(stderr, "⚠️ Removed template still accessible (error).\n");
        }
    } else {
        fprintf(stderr, "⚠️ Remove failed: %s\n", g_errBuf);
    }

    // Start main thread
    m_running = true;
    m_inicializado = true;
    m_thread = std::thread(&JsonGenerator::mainLoop, this);

    fprintf(stdout, "✅ JsonGenerator initialized successfully.\n");
    return true;
}

void JsonGenerator::shutdown() {
    m_running = false;
    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_mm) {
        if (m_templateMemory) {
            m_mm->free(0x0001, g_dummy);
            m_templateMemory = nullptr;
        }
        if (m_jsonMemory) {
            m_mm->free(0x0002, g_dummy);
            m_jsonMemory = nullptr;
        }
    }
    m_inicializado = false;
}

void JsonGenerator::mainLoop() {
    while (m_running) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::seconds(1), [this] {
            return !m_running;
        });
    }
}

} // namespace JSONWorker