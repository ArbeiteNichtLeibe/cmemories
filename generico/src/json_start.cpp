/* this script is a twin brother from shutdown.cpp if you change this one, you must change the other one!! */
/* ask for the code!! */

#include "json_start.hpp"
#include "../../jsonhandler/include/json_generator.hpp"
#include "../../jsonhandler/include/json_worker_instance.hpp"
#include "../../uteis/include/uteis.hpp"
#include "../../simpleserver/include/http_server.hpp"
#include "../include/start_tpm.hpp"
#include "../../lerconfig/include/config.hpp"
#include "../../socketmanager/include/external_buffer_socket_manager.hpp"   // <-- novo
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <chrono>
#include <arpa/inet.h>

namespace JSONStart {

// Estrutura interna para guardar os buffers alocados por slot
struct SlotBuffers {
    uint8_t* inBuf = nullptr;
    size_t   inCap = 0;
    uint8_t* outBuf = nullptr;
    size_t   outCap = 0;
};

static ExternalBufferSocketManager* g_socketManager = nullptr;
static SlotBuffers* g_slotBuffers = nullptr;          // array alocado via MemoryManagerV2
static size_t g_maxConns = 0;
static memorymanager::MemoryManagerThread* g_mm = nullptr;

// ----------------------------------------------------------------------------
// Inicialização do FakeRedis (mantido igual)
// ----------------------------------------------------------------------------
static bool ensureFakeRedisInitialized(memorymanager::MemoryManagerThread* mm,
                                       FakeRedis::FakeRedis* redis,
                                       char* outError, size_t errSize) {
    if (!mm || !redis) {
        Utils::safeCopyString(outError, errSize, "Ponteiros nulos");
        return false;
    }

    static bool initialized = false;
    if (initialized) return true;

    char errBuf[256] = {0};
    if (!redis->initialize(mm, 0, errBuf, sizeof(errBuf))) {
        Utils::safeCopyString(outError, errSize, errBuf);
        return false;
    }
    initialized = true;
    fprintf(stdout, "✅ FakeRedis initialized successfully (via JSONStart).\n");
    return true;
}

// ----------------------------------------------------------------------------
// Inicialização principal
// ----------------------------------------------------------------------------
bool init(memorymanager::MemoryManagerThread* mm,
          FakeRedis::FakeRedis* redis,
          bool runTests,
          char* outError, size_t errSize) {
    (void)runTests;  // sempre true, ignorado

    if (!mm || !redis) {
        Utils::safeCopyString(outError, errSize, "Ponteiros nulos fornecidos");
        return false;
    }
    g_mm = mm;

    // 1. FakeRedis
    if (!ensureFakeRedisInitialized(mm, redis, outError, errSize)) {
        return false;
    }

    // 2. JsonGenerator
    auto* gen = JSONWorker::JsonGenerator::getInstance();
    char errBuf[256] = {0};
    if (!gen->init(mm, redis, errBuf, sizeof(errBuf))) {
        Utils::safeCopyString(outError, errSize, errBuf);
        fprintf(stderr, "❌ Failed to initialize JsonGenerator: %s\n", errBuf);
        return false;
    }
    fprintf(stdout, "✅ JsonGenerator initialized successfully.\n");

    // 3. HttpServer
    tpm2::TPMManager& tpm_manager = TPMStart::getTPMManager();
    http::HttpServer& httpServer = http::HttpServer::getInstance();
    uint32_t httpToken = 1;
    if (!httpServer.initialize(mm, redis, &tpm_manager, httpToken,
                               errBuf, sizeof(errBuf))) {
        Utils::safeCopyString(outError, errSize, errBuf);
        fprintf(stderr, "❌ Failed to initialize HttpServer: %s\n", errBuf);
        return false;
    }
    fprintf(stdout, "✅ HttpServer initialized successfully on port %d\n",
            LerConfig::Config::getInstance().getInt("http_port", 9010));

    // ================================================================
    // 4. INICIALIZAÇÃO DO GERENCIADOR DE SOCKETS (substitui PostgreSQL)
    // ================================================================
    LerConfig::Config& config = LerConfig::Config::getInstance();

    g_maxConns = static_cast<size_t>(config.getInt("socket_max_connections", 10));
    if (g_maxConns == 0) g_maxConns = 1;

    size_t inBufSize  = static_cast<size_t>(config.getInt("socket_in_buffer_size", 4096));
    size_t outBufSize = static_cast<size_t>(config.getInt("socket_out_buffer_size", 4096));
    if (inBufSize < 1) inBufSize = 1024;
    if (outBufSize < 1) outBufSize = 1024;

    const char* socketBase = config.getString("socket_base_path", "/tmp/mysocket_");
    char socketPath[128];

    // ------------------------------------------------------------------------
    // Alocar o array de slots (SlotBuffers) via MemoryManagerV2
    // ------------------------------------------------------------------------
    size_t slotsSize = g_maxConns * sizeof(SlotBuffers);
    g_slotBuffers = static_cast<SlotBuffers*>(mm->allocate(slotsSize));
    if (!g_slotBuffers) {
        Utils::safeCopyString(outError, errSize, "Falha ao alocar slots buffers");
        return false;
    }
    // Inicializar todos os slots com zero (já feito pelo `= nullptr` na struct, mas repetimos por segurança)
    for (size_t i = 0; i < g_maxConns; ++i) {
        g_slotBuffers[i].inBuf  = nullptr;
        g_slotBuffers[i].inCap  = 0;
        g_slotBuffers[i].outBuf = nullptr;
        g_slotBuffers[i].outCap = 0;
    }

    // ------------------------------------------------------------------------
    // Alocar buffers individuais e preencher os slots
    // ------------------------------------------------------------------------
    bool allocOk = true;
    for (size_t i = 0; i < g_maxConns; ++i) {
        uint8_t* in  = static_cast<uint8_t*>(mm->allocate(inBufSize));
        uint8_t* out = static_cast<uint8_t*>(mm->allocate(outBufSize));
        if (!in || !out) {
            Utils::safeCopyString(outError, errSize, "Falha ao alocar buffers para slot");
            allocOk = false;
            break;
        }
        g_slotBuffers[i].inBuf  = in;
        g_slotBuffers[i].inCap  = inBufSize;
        g_slotBuffers[i].outBuf = out;
        g_slotBuffers[i].outCap = outBufSize;
    }

    if (!allocOk) {
        // Libera o que já foi alocado
        for (size_t i = 0; i < g_maxConns; ++i) {
            if (g_slotBuffers[i].inBuf)  mm->deallocate(g_slotBuffers[i].inBuf,  g_slotBuffers[i].inCap);
            if (g_slotBuffers[i].outBuf) mm->deallocate(g_slotBuffers[i].outBuf, g_slotBuffers[i].outCap);
        }
        mm->deallocate(g_slotBuffers, slotsSize);
        g_slotBuffers = nullptr;
        return false;
    }

    // ------------------------------------------------------------------------
    // Criar e inicializar o ExternalBufferSocketManager (placement new)
    // ------------------------------------------------------------------------
    void* memSlotMgr = mm->allocate(sizeof(ExternalBufferSocketManager));
    if (!memSlotMgr) {
        Utils::safeCopyString(outError, errSize, "Falha ao alocar SocketManager");
        // Libera buffers
        for (size_t i = 0; i < g_maxConns; ++i) {
            mm->deallocate(g_slotBuffers[i].inBuf,  g_slotBuffers[i].inCap);
            mm->deallocate(g_slotBuffers[i].outBuf, g_slotBuffers[i].outCap);
        }
        mm->deallocate(g_slotBuffers, slotsSize);
        g_slotBuffers = nullptr;
        return false;
    }
    g_socketManager = new (memSlotMgr) ExternalBufferSocketManager(g_maxConns);

    // Conectar cada slot ao seu socket path
    for (size_t i = 0; i < g_maxConns; ++i) {
        snprintf(socketPath, sizeof(socketPath), "%s%zu", socketBase, i);
        if (!g_socketManager->connectSlot(i, socketPath,
                                          g_slotBuffers[i].inBuf,
                                          g_slotBuffers[i].inCap,
                                          g_slotBuffers[i].outBuf,
                                          g_slotBuffers[i].outCap,
                                          errBuf, sizeof(errBuf))) {
            Utils::safeCopyString(outError, errSize, errBuf);
            fprintf(stderr, "❌ Failed to connect slot %zu: %s\n", i, errBuf);
            // Limpeza parcial (os buffers e manager serão liberados no shutdown)
            // Mas precisamos parar o manager se já tiver iniciado? Ainda não iniciamos.
            // Vamos fazer shutdown parcial aqui?
            // Melhor: chamar shutdown() para limpar tudo e retornar false.
            shutdown();
            return false;
        }
        fprintf(stdout, "✅ Socket slot %zu connected to %s\n", i, socketPath);
    }

    // Iniciar a thread do manager
    g_socketManager->start();
    fprintf(stdout, "✅ ExternalBufferSocketManager started with %zu connections.\n", g_maxConns);

    return true;
}

// ----------------------------------------------------------------------------
// Shutdown – libera tudo na ordem inversa
// ----------------------------------------------------------------------------
void shutdown() {
    fprintf(stdout, "ℹ️ JSONStart::shutdown() chamado – desligando SocketManager...\n");

    if (g_socketManager) {
        g_socketManager->stop();                // para a thread e fecha sockets
        g_socketManager->~ExternalBufferSocketManager(); // destrutor explícito
        if (g_mm) {
            g_mm->deallocate(g_socketManager, sizeof(ExternalBufferSocketManager));
        }
        g_socketManager = nullptr;
    }

    if (g_slotBuffers && g_mm) {
        for (size_t i = 0; i < g_maxConns; ++i) {
            if (g_slotBuffers[i].inBuf)
                g_mm->deallocate(g_slotBuffers[i].inBuf, g_slotBuffers[i].inCap);
            if (g_slotBuffers[i].outBuf)
                g_mm->deallocate(g_slotBuffers[i].outBuf, g_slotBuffers[i].outCap);
        }
        g_mm->deallocate(g_slotBuffers, g_maxConns * sizeof(SlotBuffers));
        g_slotBuffers = nullptr;
    }

    fprintf(stdout, "✅ JSONStart shutdown completo.\n");
}

} // namespace JSONStart