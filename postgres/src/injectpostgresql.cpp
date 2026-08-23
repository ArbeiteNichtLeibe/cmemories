#include "injectpostgresql.hpp"
#include "postgresql_service.hpp"
#include "postgresql_public.hpp"
#include "../../analistadebage/include/analista_bage.hpp"
#include "../../embeddings/include/embedding_processor.hpp"
#include "../../fakeredis/include/fake_redis.hpp"
#include "../../jsonworker/include/json_worker.hpp"
#include "../../utils/include/logs.hpp"
#include "../../utils/include/utf8converter.hpp"
#include <cstring>
#include <cstdio>

static void* g_service = nullptr;
static FakeRedis::FakeRedis* g_redis = nullptr;
static JSONWorker::JsonWorker* g_jsonWorker = nullptr;

void* injec_postgresql_init(MemoryManagerV2* mm, const char* path, int max,
                            char* outError, size_t errSize) {
    if (g_service) {
        snprintf(outError, errSize, "Já inicializado");
        return nullptr;
    }

    // 1. PostgreSQL
    g_service = postgresql_service_init(mm, path, max, outError, errSize);
    if (!g_service) {
        utils::log_event("ERROR", "[inject] Falha ao iniciar PostgreSQL: %s", outError);
        return nullptr;
    }

    // 2. Logger
    if (!utils::init_logger(g_service, outError, errSize)) {
        utils::log_event("ERROR", "[inject] Falha ao iniciar Logger: %s", outError);
        postgresql_service_shutdown(g_service);
        g_service = nullptr;
        return nullptr;
    }

    // 3. FakeRedis
    g_redis = &FakeRedis::FakeRedis::getInstance();
    if (!g_redis->inicializar(mm, 0x52454449, outError, errSize)) {
        utils::log_event("ERROR", "[inject] Falha ao iniciar FakeRedis: %s", outError);
        utils::shutdown_logger();
        postgresql_service_shutdown(g_service);
        g_service = nullptr;
        g_redis = nullptr;
        return nullptr;
    }

    // 4. JsonWorker
    g_jsonWorker = JSONWorker::JsonWorker::getInstance();
    if (!g_jsonWorker->inicializar(mm, g_redis, outError, errSize)) {
        utils::log_event("ERROR", "[inject] Falha ao iniciar JsonWorker: %s", outError);
        g_redis->finalizar();
        g_redis = nullptr;
        utils::shutdown_logger();
        postgresql_service_shutdown(g_service);
        g_service = nullptr;
        g_jsonWorker = nullptr;
        return nullptr;
    }

    // 4.5 Registrar template para chunks
    const char* CHUNK_TEMPLATE =
        "{\"rag_id\":[[rag_id]],\"chunk_numero\":[[chunk_numero]],\"total_chunks\":[[total_chunks]],\"conteudo\":[[conteudo]]}";
    char errTpl[256];
    if (!g_jsonWorker->registrarTemplate("chunk", CHUNK_TEMPLATE, errTpl, sizeof(errTpl))) {
        utils::log_event("ERROR", "[inject] Falha ao registrar template chunk: %s", errTpl);
        g_jsonWorker->shutdown();
        JSONWorker::JsonWorker::destroyInstance();
        g_jsonWorker = nullptr;
        g_redis->finalizar();
        g_redis = nullptr;
        utils::shutdown_logger();
        postgresql_service_shutdown(g_service);
        g_service = nullptr;
        if (outError) snprintf(outError, errSize, "Falha registrar template chunk: %s", errTpl);
        return nullptr;
    }

    // 5. UTF-8
    if (!Utils::init_utf8(mm, outError, errSize)) {
        utils::log_event("ERROR", "[inject] Falha ao iniciar UTF-8: %s", outError);
        g_jsonWorker->shutdown();
        JSONWorker::JsonWorker::destroyInstance();
        g_jsonWorker = nullptr;
        g_redis->finalizar();
        g_redis = nullptr;
        utils::shutdown_logger();
        postgresql_service_shutdown(g_service);
        g_service = nullptr;
        return nullptr;
    }

    // 6. Analista de Bagé
    if (!AnalistaDeBage::init(mm, g_service, outError, errSize)) {
        utils::log_event("ERROR", "[inject] Falha ao iniciar AnalistaDeBage: %s", outError);
        Utils::shutdown_utf8();
        g_jsonWorker->shutdown();
        JSONWorker::JsonWorker::destroyInstance();
        g_jsonWorker = nullptr;
        g_redis->finalizar();
        g_redis = nullptr;
        utils::shutdown_logger();
        postgresql_service_shutdown(g_service);
        g_service = nullptr;
        return nullptr;
    }

    // 7. Embedding Processor
    if (!EmbeddingProcessor::init(mm, g_service, outError, errSize)) {
        utils::log_event("ERROR", "[inject] Falha ao iniciar EmbeddingProcessor: %s", outError);
        AnalistaDeBage::shutdown();
        Utils::shutdown_utf8();
        g_jsonWorker->shutdown();
        JSONWorker::JsonWorker::destroyInstance();
        g_jsonWorker = nullptr;
        g_redis->finalizar();
        g_redis = nullptr;
        utils::shutdown_logger();
        postgresql_service_shutdown(g_service);
        g_service = nullptr;
        return nullptr;
    }

    utils::log_event("INFO", "[inject] Todos os serviços iniciados com sucesso (PG, Logger, FakeRedis, JsonWorker, UTF-8, Analista, Embedding)");
    fprintf(stderr, "[inject] ✅ Serviços iniciados (PG, Logger, FakeRedis, JsonWorker, UTF-8, Analista, Embedding)\n");
    return g_service;
}

void injec_postgresql_shutdown(void* svc) {
    if (svc) {
        EmbeddingProcessor::shutdown();
        AnalistaDeBage::shutdown();
        Utils::shutdown_utf8();
        if (g_jsonWorker) {
            g_jsonWorker->shutdown();
            JSONWorker::JsonWorker::destroyInstance();
            g_jsonWorker = nullptr;
        }
        if (g_redis) {
            g_redis->finalizar();
            g_redis = nullptr;
        }
        utils::shutdown_logger();
        postgresql_service_shutdown(svc);
        if (svc == g_service) g_service = nullptr;
        utils::log_event("INFO", "[inject] Todos os serviços finalizados");
        fprintf(stderr, "[inject] ✅ Serviços finalizados (PG, Logger, FakeRedis, JsonWorker, UTF-8, Analista, Embedding)\n");
    }
}

// ============================================================
// GETTERS
// ============================================================
int injec_postgresql_get_max_connections() {
    return postgresql_get_max_connections(g_service);
}

void injec_postgresql_get_pg_socket_path(char* buffer, size_t size) {
    postgresql_get_pg_socket_path(g_service, buffer, size);
}

void injec_postgresql_get_logs_path(char* buffer, size_t size) {
    postgresql_get_logs_path(g_service, buffer, size);
}

int injec_postgresql_get_keepalive_seconds() {
    return postgresql_get_keepalive_seconds(g_service);
}

int injec_postgresql_get_reconnect_delay_ms() {
    return postgresql_get_reconnect_delay_ms(g_service);
}

int injec_postgresql_get_conexoes_abertas(int* ids, int maxIds) {
    return postgresql_get_active_listener_ids(g_service, ids, maxIds);
}