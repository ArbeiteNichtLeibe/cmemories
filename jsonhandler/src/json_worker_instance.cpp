// jsonhandler/src/json_worker_instance.cpp
#include "../include/json_worker_instance.hpp"
#include "../../jsonhandler/include/json_generator.hpp"
#include "../include/config_template.hpp"
#include "../../uteis/include/uteis.hpp"

#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <random>
#include <chrono>

namespace JSONWorker {

// ------------------------------------------------------------
// 1. Parsing manual da requisição JSON (sem arrays na stack)
// ------------------------------------------------------------
static bool parseRequest(const char* requestBuf,
                         const char*& outTemplateName, size_t& outTplLen,
                         const char*& outDados, size_t& outDadosLen,
                         char* outError, size_t errSize) {
    if (!requestBuf) {
        Utils::safeCopyString(outError, errSize, "Requisição vazia");
        return false;
    }

    // Buscar "template":" ... "
    const char* tplKey = "\"template\":\"";
    const char* tplStart = strstr(requestBuf, tplKey);
    if (!tplStart) {
        Utils::safeCopyString(outError, errSize, "Campo 'template' não encontrado");
        return false;
    }
    tplStart += strlen(tplKey);
    const char* tplEnd = strchr(tplStart, '"');
    if (!tplEnd) {
        Utils::safeCopyString(outError, errSize, "Valor do template mal formatado");
        return false;
    }
    outTemplateName = tplStart;
    outTplLen = tplEnd - tplStart;

    // Buscar "dados": { ... }
    const char* dadosKey = "\"dados\":";
    const char* dadosStart = strstr(tplEnd, dadosKey);
    if (!dadosStart) {
        Utils::safeCopyString(outError, errSize, "Campo 'dados' não encontrado");
        return false;
    }
    dadosStart += strlen(dadosKey);
    const char* dadosEnd = strrchr(dadosStart, '}');
    if (!dadosEnd) {
        Utils::safeCopyString(outError, errSize, "Dados mal formatados (faltando '}')");
        return false;
    }
    outDados = dadosStart;
    outDadosLen = dadosEnd - dadosStart + 1;
    return true;
}

// ------------------------------------------------------------
// 2. Substituição de placeholders (<nome>) – sem arrays na stack
// ------------------------------------------------------------
static bool applyTemplate(const char* templateContent, size_t tplLen,
                          const char* dadosJson, size_t dadosLen,
                          char* outBuffer, size_t outSize,
                          char* outError, size_t errSize) {
    (void)dadosLen;
    if (!templateContent || !dadosJson || !outBuffer) {
        Utils::safeCopyString(outError, errSize, "Parâmetros inválidos");
        return false;
    }

    if (tplLen >= outSize) {
        Utils::safeCopyString(outError, errSize, "Template muito grande para o buffer");
        return false;
    }
    // Copia o template para o buffer de saída
    memcpy(outBuffer, templateContent, tplLen);
    outBuffer[tplLen] = '\0';

    char* work = outBuffer;
    int maxReplacements = 100;
    int replacements = 0;

    while (replacements < maxReplacements) {
        // Procurar por placeholder no formato <nome>
        char* open = strstr(work, "<");
        if (!open) break;
        char* close = strstr(open + 1, ">");
        if (!close) break;

        // Extrair o nome do campo (o que está entre < e >)
        size_t keyLen = close - open - 1;
        if (keyLen == 0) {
            work = close + 1;
            continue;
        }

        // Procurar no dadosJson: "campo":"valor"
        const char* ptr = dadosJson;
        const char* valStart = nullptr;
        const char* valEnd = nullptr;

        while (*ptr) {
            if (*ptr == '"') {
                const char* keyPtr = ptr + 1;
                // Comparar caractere a caractere com o nome extraído
                size_t i = 0;
                while (i < keyLen && keyPtr[i] == open[1 + i]) i++;
                if (i == keyLen && keyPtr[keyLen] == '"' &&
                    keyPtr[keyLen + 1] == ':' && keyPtr[keyLen + 2] == '"') {
                    valStart = keyPtr + keyLen + 3; // após '"' + ':' + '"'
                    valEnd = strchr(valStart, '"');
                    if (valEnd) break;
                }
            }
            ptr++;
        }

        if (valStart && valEnd) {
            size_t valLen = valEnd - valStart;
            size_t prefixLen = open - outBuffer;
            size_t suffixLen = strlen(close + 1);
            size_t newLen = prefixLen + valLen + suffixLen + 1;
            if (newLen >= outSize) {
                Utils::safeCopyString(outError, errSize, "JSON final excede o buffer");
                return false;
            }
            memmove(outBuffer + prefixLen + valLen, close + 1, suffixLen + 1);
            memcpy(outBuffer + prefixLen, valStart, valLen);
            work = outBuffer + prefixLen + valLen;
            replacements++;
        } else {
            // Campo não encontrado – manter placeholder
            work = close + 1;
        }
    }

    if (replacements >= maxReplacements) {
        Utils::safeCopyString(outError, errSize, "Limite de substituições excedido");
        return false;
    }

    return true;
}

// ------------------------------------------------------------
// 3. Função pública processRequest
// ------------------------------------------------------------
bool processRequest(int64_t requestId,
                    memorymanager::MemoryManagerThread* mm,
                    FakeRedis::FakeRedis* redis,
                    int64_t* outResultId,
                    char* outError, size_t errSize) {
    if (!mm || !redis) {
        Utils::safeCopyString(outError, errSize, "Parâmetros nulos");
        return false;
    }

    // Alocar 100 MB da arena (com ID aleatório)
    uint64_t threadId = static_cast<uint64_t>(pthread_self()) ^
                        static_cast<uint64_t>(std::random_device{}());
    const uint32_t MB = 100;
    uint32_t startBlock = 0;
    void* startAddr = nullptr;
    void* endAddr = nullptr;
    if (!mm->allocate(threadId, MB, startBlock, startAddr, endAddr, outError)) {
        return false;
    }

    // Dividir os 100 MB em áreas
    char* reqBuf = static_cast<char*>(startAddr);
    size_t reqSize = 10 * 1024 * 1024;
    char* tplBuf = reqBuf + reqSize;
    size_t tplSize = 10 * 1024 * 1024;
    char* outBuf = tplBuf + tplSize;
    size_t outSize = 80 * 1024 * 1024;

    // Área para erros (dentro da arena)
    char* errBuf = outBuf + outSize - 256;
    size_t errBufSize = 256;

    // --- Ler requisição do Redis ---
    if (!redis->get(requestId, reqBuf, reqSize, errBuf, errBufSize)) {
        Utils::safeCopyString(outError, errSize, errBuf);
        mm->free(threadId, errBuf);
        return false;
    }

    // --- Parse da requisição ---
    const char* templateName = nullptr;
    size_t tplNameLen = 0;
    const char* dadosRaw = nullptr;
    size_t dadosLen = 0;
    if (!parseRequest(reqBuf, templateName, tplNameLen, dadosRaw, dadosLen,
                      errBuf, errBufSize)) {
        Utils::safeCopyString(outError, errSize, errBuf);
        mm->free(threadId, errBuf);
        return false;
    }

    // Copiar nome do template para a arena
    char* tplNameBuf = tplBuf;
    if (tplNameLen >= tplSize) {
        Utils::safeCopyString(outError, errSize, "Nome do template muito longo");
        mm->free(threadId, errBuf);
        return false;
    }
    memcpy(tplNameBuf, templateName, tplNameLen);
    tplNameBuf[tplNameLen] = '\0';

    // --- Obter a memória de templates do JsonGenerator ---
    auto* gen = JSONWorker::JsonGenerator::getInstance();
    void* templateMemory = gen->getTemplateMemory();
    size_t templateMemorySize = gen->getTemplateMemorySize();

    if (!templateMemory || templateMemorySize == 0) {
        Utils::safeCopyString(errBuf, errBufSize, "JsonGenerator não possui memória de templates");
        mm->free(threadId, errBuf);
        Utils::safeCopyString(outError, errSize, errBuf);
        return false;
    }

    // --- Obter o conteúdo do template (usando a memória do JsonGenerator) ---
    char* tplContentBuf = tplBuf + tplNameLen + 1;
    size_t tplContentSize = tplSize - (tplNameLen + 1);
    if (!consultar_template(templateMemory, templateMemorySize,
                            tplNameBuf,
                            tplContentBuf, tplContentSize,
                            errBuf, errBufSize)) {
        Utils::safeCopyString(outError, errSize, errBuf);
        mm->free(threadId, errBuf);
        return false;
    }

    // --- Gerar JSON final ---
    if (!applyTemplate(tplContentBuf, strlen(tplContentBuf),
                       dadosRaw, dadosLen,
                       outBuf, outSize,
                       errBuf, errBufSize)) {
        Utils::safeCopyString(outError, errSize, errBuf);
        mm->free(threadId, errBuf);
        return false;
    }

    // --- Gravar JSON final no Redis (TTL 3 min) ---
    const uint32_t TTL_MS = 3 * 60 * 1000;
    int64_t resultId = redis->set(std::string_view(outBuf, strlen(outBuf)),
                                  TTL_MS, errBuf, errBufSize);
    if (resultId < 0) {
        Utils::safeCopyString(outError, errSize, errBuf);
        mm->free(threadId, errBuf);
        return false;
    }

    if (outResultId) {
        *outResultId = resultId;
    }

    // --- Remover chave de requisição ---
    if (!redis->del(requestId, errBuf, errBufSize)) {
        fprintf(stderr, "Aviso: falha ao remover chave de requisição: %s\n", errBuf);
    }

    // --- Liberar memória alocada ---
    if (!mm->free(threadId, errBuf)) {
        fprintf(stderr, "Aviso: falha ao liberar memória: %s\n", errBuf);
    }

    return true;
}

} // namespace JSONWorker