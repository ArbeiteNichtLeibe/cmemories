#include "json_start.hpp"
#include "../../jsonhandler/include/json_generator.hpp"
#include "../../jsonhandler/include/json_worker_instance.hpp"
#include "../../uteis/include/uteis.hpp"
#include "../../simpleserver/include/http_server.hpp"
#include "../include/start_tpm.hpp"
#include "../../lerconfig/include/config.hpp"
#include "../../postgres/include/postgres_pool_manager.hpp"
#include <cstdio>
#include <cstring>
#include <pthread.h>

namespace JSONStart {

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

bool init(memorymanager::MemoryManagerThread* mm,
          FakeRedis::FakeRedis* redis,
          bool runTests,
          char* outError, size_t errSize) {
    (void)runTests;

    if (!mm || !redis) {
        Utils::safeCopyString(outError, errSize, "Ponteiros nulos fornecidos");
        return false;
    }

    if (!ensureFakeRedisInitialized(mm, redis, outError, errSize)) {
        return false;
    }

    auto* gen = JSONWorker::JsonGenerator::getInstance();
    char errBuf[256] = {0};

    if (!gen->init(mm, redis, errBuf, sizeof(errBuf))) {
        Utils::safeCopyString(outError, errSize, errBuf);
        fprintf(stderr, "❌ Failed to initialize JsonGenerator: %s\n", errBuf);
        return false;
    }
    fprintf(stdout, "✅ JsonGenerator initialized successfully.\n");

    const char* templateNome = "usuario_teste";
    const char* templateConteudo = "{\"nome\":\"<nome>\",\"senha\":\"<senha>\"}";

    if (!gen->addTemplate(templateNome, templateConteudo, errBuf, sizeof(errBuf))) {
        Utils::safeCopyString(outError, errSize, errBuf);
        fprintf(stderr, "❌ Failed to add test template: %s\n", errBuf);
        JSONWorker::JsonGenerator::destroyInstance();
        return false;
    }
    fprintf(stdout, "✅ Test template '%s' added successfully.\n", templateNome);

    fprintf(stdout, "\n🧪 Executando teste de processamento de requisição...\n");
    const char* requestJson = "{\"template\":\"usuario_teste\",\"dados\":{\"nome\":\"Maria\",\"senha\":\"123456\"}}";
    int64_t requestId = redis->set(std::string_view(requestJson, strlen(requestJson)), 60000, errBuf, sizeof(errBuf));
    if (requestId < 0) {
        fprintf(stderr, "❌ Falha ao gravar requisição de teste no Redis: %s\n", errBuf);
    } else {
        fprintf(stdout, "✅ Requisição de teste gravada no Redis com ID: %lld\n", (long long)requestId);

        int64_t resultId = -1;
        if (JSONWorker::processRequest(requestId, mm, redis, &resultId, errBuf, sizeof(errBuf))) {
            fprintf(stdout, "✅ Requisição processada com sucesso. ID do resultado: %lld\n", (long long)resultId);

            uint64_t token = static_cast<uint64_t>(pthread_self()) ^ 0x12345678ULL;
            const uint32_t BUFFER_MB = 1;
            uint32_t startBlock = 0;
            void* startAddr = nullptr;
            void* endAddr = nullptr;
            char allocErr[256] = {0};

            if (mm->allocate(token, BUFFER_MB, startBlock, startAddr, endAddr, allocErr)) {
                char* resultBuffer = static_cast<char*>(startAddr);
                if (redis->get(resultId, resultBuffer, BUFFER_MB * 1024 * 1024, errBuf, sizeof(errBuf))) {
                    fprintf(stdout, "📄 JSON gerado:\n%s\n", resultBuffer);
                } else {
                    fprintf(stderr, "❌ Falha ao ler resultado: %s\n", errBuf);
                }
                mm->free(token, allocErr);
            } else {
                fprintf(stderr, "❌ Falha ao alocar buffer para resultado: %s\n", allocErr);
            }

            redis->del(resultId, errBuf, sizeof(errBuf));
        } else {
            fprintf(stderr, "❌ Falha no processRequest: %s\n", errBuf);
        }

        redis->del(requestId, errBuf, sizeof(errBuf));
    }
    fprintf(stdout, "🧪 Teste concluído.\n\n");

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

    // ============================================================
    // INICIALIZAÇÃO DO POSTGRESQL POOL (COM CÓPIA DEFENSIVA)
    // ============================================================
    LerConfig::Config& config = LerConfig::Config::getInstance();

    char dbHost[64];
    char dbName[64];
    char dbUser[64];
    char dbPassword[64];

    const char* tmp = config.getString("db_host", "localhost");
    strncpy(dbHost, (tmp && tmp[0] != '\0') ? tmp : "localhost", sizeof(dbHost) - 1);
    dbHost[sizeof(dbHost) - 1] = '\0';

    tmp = config.getString("db_name", "postgres");
    strncpy(dbName, (tmp && tmp[0] != '\0') ? tmp : "postgres", sizeof(dbName) - 1);
    dbName[sizeof(dbName) - 1] = '\0';

    tmp = config.getString("db_user", "postgres");
    strncpy(dbUser, (tmp && tmp[0] != '\0') ? tmp : "postgres", sizeof(dbUser) - 1);
    dbUser[sizeof(dbUser) - 1] = '\0';

    tmp = config.getString("db_password", "");
    strncpy(dbPassword, tmp ? tmp : "", sizeof(dbPassword) - 1);
    dbPassword[sizeof(dbPassword) - 1] = '\0';

    int dbPort = config.getInt("db_port", 5432);
    int poolSize = config.getInt("db_pool_size", 5);
    if (poolSize < 1) poolSize = 1;
    if (poolSize > 10) poolSize = 10;

    fprintf(stdout, "📡 PostgreSQL config: host=%s port=%d db=%s user=%s\n",
            dbHost, dbPort, dbName, dbUser);

    if (!PostgresPoolManager::getInstance().initialize(mm,
                                                       dbHost, dbPort,
                                                       dbName, dbUser,
                                                       dbPassword,
                                                       poolSize,
                                                       errBuf, sizeof(errBuf))) {
        Utils::safeCopyString(outError, errSize, errBuf);
        fprintf(stderr, "❌ Failed to initialize PostgreSQL pool: %s\n", errBuf);
        return false;
    }
    fprintf(stdout, "✅ PostgreSQL pool initialized with %d connections.\n", poolSize);

    // ============================================================
    // TESTES SQL COM O POSTGRESQL
    // ============================================================
    fprintf(stdout, "\n🧪 Executando testes SQL no PostgreSQL...\n");
    PostgresSession* sess = PostgresPoolManager::getInstance().getConnection(0);
    if (sess && sess->isReady()) {
        char sqlErr[256];

        const char* createTable = "CREATE TABLE IF NOT EXISTS test_sistema (id SERIAL PRIMARY KEY, nome TEXT, criado TIMESTAMP DEFAULT NOW());";
        if (sess->sendQuery(createTable, strlen(createTable), sqlErr, sizeof(sqlErr))) {
            fprintf(stdout, "✅ Tabela 'test_sistema' criada/verificada.\n");
        } else {
            fprintf(stderr, "❌ Falha ao criar tabela: %s\n", sqlErr);
        }

        const char* insertData = "INSERT INTO test_sistema (nome) VALUES ('Andre'), ('Maria'), ('João');";
        if (sess->sendQuery(insertData, strlen(insertData), sqlErr, sizeof(sqlErr))) {
            fprintf(stdout, "✅ Dados inseridos na tabela.\n");
        } else {
            fprintf(stderr, "❌ Falha ao inserir dados: %s\n", sqlErr);
        }

        const char* selectData = "SELECT id, nome, criado FROM test_sistema ORDER BY id;";
        if (sess->sendQuery(selectData, strlen(selectData), sqlErr, sizeof(sqlErr))) {
            fprintf(stdout, "✅ Consulta SELECT enviada. Aguarde resposta...\n");
        } else {
            fprintf(stderr, "❌ Falha ao consultar dados: %s\n", sqlErr);
        }

        const char* dropTable = "DROP TABLE IF EXISTS test_sistema;";
        if (sess->sendQuery(dropTable, strlen(dropTable), sqlErr, sizeof(sqlErr))) {
            fprintf(stdout, "✅ Tabela 'test_sistema' removida (se existia).\n");
        } else {
            fprintf(stderr, "❌ Falha ao remover tabela: %s\n", sqlErr);
        }

        fprintf(stdout, "✅ Testes SQL concluídos.\n\n");
    } else {
        fprintf(stderr, "❌ Conexão 0 não está pronta para testes SQL.\n");
    }

    return true;
}

void shutdown() {
    fprintf(stdout, "ℹ️ JSONStart::shutdown() chamado (não faz nada).\n");
}

} // namespace JSONStart