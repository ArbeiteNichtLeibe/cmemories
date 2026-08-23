//    ../src/json_worker.cpp
#include "../include/json_worker.hpp"
#include "../../fakeredis/include/fake_redis.hpp"
#include "../../memorymanager/include/memory_manager_thread.hpp"

#include <cstring>
#include <cstdio>
#include <chrono>
#include <atomic>
#include <thread>
#include <algorithm>   // for std::fill_n

namespace JSONWorker {

// Offsets inside the arena (all in bytes) – must be inside the namespace
#define OFFSET_HEADER       0
#define OFFSET_TEMPLATES    (sizeof(ArenaHeader))
#define OFFSET_QUEUE        (OFFSET_TEMPLATES + MAX_TEMPLATES * sizeof(TemplateSlot))
#define OFFSET_RESULTS      (OFFSET_QUEUE + MAX_QUEUE * sizeof(RequestSlot))
#define OFFSET_BUFFER       (OFFSET_RESULTS + MAX_RESULTS * sizeof(ResultSlot))
#define BUFFER_SIZE         (ARENA_SIZE - OFFSET_BUFFER)

// Number of 1‑MiB blocks needed for the arena (uses ARENA_SIZE from the namespace)
static constexpr uint32_t ARENA_BLOCKS = ARENA_SIZE / (1024 * 1024);

// ----- Singleton -----
JsonWorker* JsonWorker::singleton = nullptr;
std::mutex JsonWorker::instanceMutex;

JsonWorker* JsonWorker::getInstance() {
    std::lock_guard<std::mutex> lock(instanceMutex);
    if (singleton == nullptr) {
        singleton = new JsonWorker();
    }
    return singleton;
}

void JsonWorker::destroyInstance() {
    std::lock_guard<std::mutex> lock(instanceMutex);
    delete singleton;
    singleton = nullptr;
}

// ----- Constructor / Destructor -----
// IMPORTANT: initialization order must match declaration order in the class
JsonWorker::JsonWorker()
    : m_mm(nullptr)
    , m_redis(nullptr)
    , m_arenaBase(nullptr)
    , m_header(nullptr)
    , m_running(true)          // declared before m_inicializado
    , m_inicializado(false) {
}

JsonWorker::~JsonWorker() {
    shutdown();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    if (m_arenaBase && m_mm) {
        char err[128];
        if (!m_mm->free(0, err)) {   // token 0 – we didn't store one
            fprintf(stderr, "[JsonWorker] Warning: free arena failed: %s\n", err);
        }
        m_arenaBase = nullptr;
    }
}

// ----- Initialization (now with MemoryManagerThread) -----
bool JsonWorker::inicializar(memorymanager::MemoryManagerThread* mm,
                             FakeRedis::FakeRedis* redis,
                             char* outError, size_t errSize) {
    if (m_inicializado) return true;
    if (!mm || !redis) {
        if (outError) snprintf(outError, errSize, "Parâmetros nulos");
        return false;
    }
    m_mm = mm;
    m_redis = redis;

    // Allocate the arena via MemoryManagerThread (1‑MiB blocks)
    uint32_t startBlock = 0;
    void* startAddr = nullptr;
    void* endAddr = nullptr;
    if (!m_mm->allocate(0, ARENA_BLOCKS, startBlock, startAddr, endAddr, outError)) {
        return false;
    }
    m_arenaBase = startAddr;

    if (!initArena(outError, errSize)) {
        char dummy[128];
        m_mm->free(0, dummy);
        m_arenaBase = nullptr;
        return false;
    }

    m_inicializado = true;
    m_workerThread = std::thread(&JsonWorker::workerLoop, this);
    return true;
}

bool JsonWorker::initArena(char* outError, size_t errSize) {
    (void)outError; (void)errSize;
    // Zero the entire arena (safe, as it is POD)
    std::fill_n(static_cast<uint8_t*>(m_arenaBase), ARENA_SIZE, 0);
    m_header = static_cast<ArenaHeader*>(m_arenaBase);
    m_header->queueHead = 0;
    m_header->queueTail = 0;
    m_header->queueCount = 0;
    m_header->totalProcessados = 0;
    m_header->totalErros = 0;
    return true;
}

// ----- Arena slot accessors (const and non‑const) -----
TemplateSlot* JsonWorker::getTemplate(int idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= MAX_TEMPLATES) return nullptr;
    uint8_t* base = static_cast<uint8_t*>(m_arenaBase);
    return reinterpret_cast<TemplateSlot*>(base + OFFSET_TEMPLATES + idx * sizeof(TemplateSlot));
}

const TemplateSlot* JsonWorker::getTemplate(int idx) const {
    if (idx < 0 || static_cast<size_t>(idx) >= MAX_TEMPLATES) return nullptr;
    const uint8_t* base = static_cast<const uint8_t*>(m_arenaBase);
    return reinterpret_cast<const TemplateSlot*>(base + OFFSET_TEMPLATES + idx * sizeof(TemplateSlot));
}

ResultSlot* JsonWorker::getResult(int idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= MAX_RESULTS) return nullptr;
    uint8_t* base = static_cast<uint8_t*>(m_arenaBase);
    return reinterpret_cast<ResultSlot*>(base + OFFSET_RESULTS + idx * sizeof(ResultSlot));
}

const ResultSlot* JsonWorker::getResult(int idx) const {
    if (idx < 0 || static_cast<size_t>(idx) >= MAX_RESULTS) return nullptr;
    const uint8_t* base = static_cast<const uint8_t*>(m_arenaBase);
    return reinterpret_cast<const ResultSlot*>(base + OFFSET_RESULTS + idx * sizeof(ResultSlot));
}

RequestSlot* JsonWorker::getQueueSlot(size_t idx) {
    uint8_t* base = static_cast<uint8_t*>(m_arenaBase);
    return reinterpret_cast<RequestSlot*>(base + OFFSET_QUEUE + idx * sizeof(RequestSlot));
}

// ----- Slot management -----
int JsonWorker::allocTemplateSlot() {
    for (size_t i = 0; i < MAX_TEMPLATES; ++i) {
        TemplateSlot* slot = getTemplate(static_cast<int>(i));
        if (!slot->usado) {
            slot->usado = true;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void JsonWorker::freeTemplateSlot(int idx) {
    TemplateSlot* slot = getTemplate(idx);
    if (slot) slot->usado = false;
}

int JsonWorker::allocResultSlot() {
    for (size_t i = 0; i < MAX_RESULTS; ++i) {
        ResultSlot* slot = getResult(static_cast<int>(i));
        if (slot->estado == 0) {
            slot->estado = 1; // processing
            return static_cast<int>(i);
        }
    }
    return -1;
}

void JsonWorker::freeResultSlot(int idx) {
    ResultSlot* slot = getResult(idx);
    if (slot) {
        slot->estado = 0;
        slot->id = 0;
    }
}

int JsonWorker::findTemplateByName(const char* name) const {
    for (size_t i = 0; i < MAX_TEMPLATES; ++i) {
        const TemplateSlot* slot = getTemplate(static_cast<int>(i));
        if (slot->usado && strncmp(slot->nome, name, MAX_TEMPLATE_NAME) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ----- Template registration -----
bool JsonWorker::registrarTemplate(const char* nome, const char* conteudo,
                                   char* outError, size_t errSize) {
    if (!m_inicializado) {
        if (outError) snprintf(outError, errSize, "JsonWorker não inicializado");
        return false;
    }
    int idx = findTemplateByName(nome);
    if (idx < 0) {
        idx = allocTemplateSlot();
        if (idx < 0) {
            if (outError) snprintf(outError, errSize, "Limite de templates excedido");
            return false;
        }
        strncpy(getTemplate(idx)->nome, nome, MAX_TEMPLATE_NAME - 1);
        getTemplate(idx)->nome[MAX_TEMPLATE_NAME - 1] = '\0';
    }
    strncpy(getTemplate(idx)->conteudo, conteudo, MAX_TEMPLATE_CONTENT - 1);
    getTemplate(idx)->conteudo[MAX_TEMPLATE_CONTENT - 1] = '\0';
    return true;
}

void JsonWorker::removerTemplate(const char* nome) {
    int idx = findTemplateByName(nome);
    if (idx >= 0) freeTemplateSlot(idx);
}

// ----- Escape (no heap) -----
static void escapeJson(const char* src, char* dst, size_t dstSize) {
    if (!src || !dst || dstSize == 0) return;
    size_t i = 0, j = 0;
    while (src[i] && j < dstSize - 1) {
        char c = src[i];
        if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
            if (j + 2 >= dstSize) break;
            dst[j++] = '\\';
            switch (c) {
                case '"':  dst[j++] = '"'; break;
                case '\\': dst[j++] = '\\'; break;
                case '\b': dst[j++] = 'b'; break;
                case '\f': dst[j++] = 'f'; break;
                case '\n': dst[j++] = 'n'; break;
                case '\r': dst[j++] = 'r'; break;
                case '\t': dst[j++] = 't'; break;
                default: dst[j++] = c; break;
            }
        } else if (static_cast<unsigned char>(c) < 0x20) {
            if (j + 6 >= dstSize) break;
            snprintf(dst + j, dstSize - j, "\\u%04x", (unsigned char)c);
            j += 6;
        } else {
            dst[j++] = c;
        }
        i++;
    }
    dst[j] = '\0';
}

// ----- Build JSON (uses the arena buffer) -----
char* JsonWorker::buildJson(const char* templateName,
                            const char* dados,
                            size_t dadosLen,
                            size_t& outSize,
                            char* outError, size_t errSize) {
    (void)dadosLen;
    int idx = findTemplateByName(templateName);
    if (idx < 0) {
        if (outError) snprintf(outError, errSize, "Template não encontrado: %s", templateName);
        return nullptr;
    }
    TemplateSlot* tpl = getTemplate(idx);

    uint8_t* bufferBase = static_cast<uint8_t*>(m_arenaBase) + OFFSET_BUFFER;
    size_t bufferSize = BUFFER_SIZE;
    char* trabalho = reinterpret_cast<char*>(bufferBase);

    size_t tplLen = strnlen(tpl->conteudo, MAX_TEMPLATE_CONTENT);
    if (tplLen >= bufferSize) {
        if (outError) snprintf(outError, errSize, "Template muito grande");
        return nullptr;
    }
    memcpy(trabalho, tpl->conteudo, tplLen + 1);

    size_t replacements = 0;               // changed from int to size_t
    bool mudou = false;
    int depth = 0;
    const int MAX_DEPTH = 20;

    do {
        mudou = false;
        depth++;
        if (depth > MAX_DEPTH) {
            if (outError) snprintf(outError, errSize, "Profundidade máxima excedida");
            return nullptr;
        }

        char* pos = trabalho;
        while (*pos) {
            char* openDouble = strstr(pos, "[[");
            char* openSingle = strstr(pos, "{{");
            char* openBrace = nullptr;
            bool isDouble = false;
            if (openDouble && openSingle) {
                if (openDouble < openSingle) { openBrace = openDouble; isDouble = true; }
                else { openBrace = openSingle; isDouble = false; }
            } else if (openDouble) { openBrace = openDouble; isDouble = true; }
            else if (openSingle) { openBrace = openSingle; isDouble = false; }
            else break;

            char* closeBrace = nullptr;
            if (isDouble) {
                closeBrace = strstr(openBrace + 2, "]]");
                if (!closeBrace) break;
            } else {
                closeBrace = strstr(openBrace + 2, "}}");
                if (!closeBrace) break;
            }

            char placeholderName[MAX_TEMPLATE_NAME];
            size_t nameLen = closeBrace - openBrace - 2;
            if (nameLen >= MAX_TEMPLATE_NAME) break;
            strncpy(placeholderName, openBrace + 2, nameLen);
            placeholderName[nameLen] = '\0';

            const char* dataPtr = dados;
            const char* valor = nullptr;
            size_t valorLen = 0;
            while (*dataPtr) {
                while (*dataPtr == '&' || *dataPtr == ';' || *dataPtr == ' ') dataPtr++;
                if (*dataPtr == '\0') break;
                const char* chaveStart = dataPtr;
                const char* igual = strchr(dataPtr, '=');
                if (!igual) break;
                if (strncmp(chaveStart, placeholderName, igual - chaveStart) == 0) {
                    valor = igual + 1;
                    const char* fim = strpbrk(valor, "&;");
                    if (fim) valorLen = fim - valor;
                    else valorLen = strlen(valor);
                    break;
                }
                dataPtr = strpbrk(dataPtr, "&;");
                if (!dataPtr) break;
                dataPtr++;
            }

            if (valor && valorLen > 0) {
                char valorBuffer[8192];
                if (isDouble) {
                    escapeJson(valor, valorBuffer, sizeof(valorBuffer));
                } else {
                    strncpy(valorBuffer, valor, sizeof(valorBuffer) - 1);
                    valorBuffer[sizeof(valorBuffer) - 1] = '\0';
                }
                size_t valorStrLen = strlen(valorBuffer);
                size_t prefixLen = openBrace - trabalho;
                size_t suffixLen = strlen(closeBrace + 2);
                size_t newLen = prefixLen + valorStrLen + suffixLen + 1;
                if (newLen >= bufferSize) {
                    if (outError) snprintf(outError, errSize, "JSON excede o buffer da arena");
                    return nullptr;
                }
                memmove(trabalho + prefixLen + valorStrLen, closeBrace + 2, suffixLen + 1);
                memcpy(trabalho + prefixLen, valorBuffer, valorStrLen);
                mudou = true;
                replacements++;
                if (replacements >= MAX_REPLACEMENTS) {
                    if (outError) snprintf(outError, errSize, "Excedeu limite de substituições");
                    return nullptr;
                }
                pos = trabalho + prefixLen + valorStrLen;
            } else {
                pos = closeBrace + 2;
            }
        }
    } while (mudou);

    outSize = strlen(trabalho) + 1;
    if (outSize > MAX_JSON_SIZE) {
        if (outError) snprintf(outError, errSize, "JSON excede tamanho máximo (%zu)", MAX_JSON_SIZE);
        return nullptr;
    }
    return trabalho;
}

// ----- Process a request (worker thread) -----
void JsonWorker::processRequest(const RequestSlot& req) {
    size_t jsonSize = 0;
    char erroBuf[256] = {0};
    char* jsonPtr = buildJson(req.templateName, req.dados, strnlen(req.dados, MAX_REQUEST_DATA),
                              jsonSize, erroBuf, sizeof(erroBuf));

    bool success = false;
    int64_t redisId = -1;
    char redisErrMsg[256] = {0};

    if (jsonPtr != nullptr) {
        constexpr uint32_t TTL_15_MIN = 15 * 60 * 1000;
        // Try to store in FakeRedis (up to 3 attempts)
        for (int attempt = 0; attempt < 3; ++attempt) {
            char errRedis[256];
            redisId = m_redis->set(std::string_view(jsonPtr, jsonSize - 1), TTL_15_MIN,
                                   errRedis, sizeof(errRedis));
            if (redisId >= 0) {
                success = true;
                break;
            }
            strncpy(redisErrMsg, errRedis, sizeof(redisErrMsg) - 1);
            redisErrMsg[sizeof(redisErrMsg) - 1] = '\0';
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!success) {
            snprintf(erroBuf, sizeof(erroBuf), "Falha no Redis: %s", redisErrMsg);
        }
    } else {
        // buildJson already filled erroBuf
        success = false;
    }

    // Find the corresponding result slot (by temporary ID)
    int resultIdx = -1;
    for (size_t i = 0; i < MAX_RESULTS; ++i) {
        ResultSlot* slot = getResult(static_cast<int>(i));
        if (slot->estado != 0 && slot->id == req.id) {
            resultIdx = static_cast<int>(i);
            break;
        }
    }

    if (resultIdx >= 0) {
        ResultSlot* res = getResult(resultIdx);
        if (success) {
            res->id = redisId;
            res->estado = 2;          // ready
            res->erro = false;
            res->erroMsg[0] = '\0';
        } else {
            res->estado = 1;          // processed with error
            res->erro = true;
            strncpy(res->erroMsg, erroBuf, MAX_ERROR_MSG - 1);
            res->erroMsg[MAX_ERROR_MSG - 1] = '\0';
        }
        res->completedAt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();

        if (success) {
            m_header->totalProcessados++;      // CORRECT: no "atomic" prefix
        } else {
            m_header->totalErros++;
        }
    } else {
        // Slot not found – internal error
        m_header->totalErros++;
    }
}

// ----- Worker loop -----
void JsonWorker::workerLoop() {
    while (m_running) {
        RequestSlot req;
        bool hasWork = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return m_header->queueCount.load() > 0 || !m_running;   // CORRECT
            });
            if (!m_running) break;
            if (m_header->queueCount.load() == 0) continue;             // CORRECT

            size_t idx = m_header->queueHead % MAX_QUEUE;
            RequestSlot* slot = getQueueSlot(idx);
            memcpy(&req, slot, sizeof(RequestSlot));
            std::fill_n(reinterpret_cast<uint8_t*>(slot), sizeof(RequestSlot), 0);
            m_header->queueHead++;
            m_header->queueCount--;                                     // CORRECT
            hasWork = true;
        }
        if (hasWork) {
            processRequest(req);
        }
    }
}

// ----- Public API -----
JsonId JsonWorker::solicitarJson(const char* templateName, const char* dados, size_t dadosLen,
                                 char* outError, size_t errSize) {
    if (!m_inicializado) {
        if (outError) snprintf(outError, errSize, "JsonWorker não inicializado");
        return -1;
    }
    if (findTemplateByName(templateName) < 0) {
        if (outError) snprintf(outError, errSize, "Template não encontrado: %s", templateName);
        return -1;
    }

    // Reserve a result slot with a temporary ID (negative)
    int resultIdx = allocResultSlot();
    if (resultIdx < 0) {
        if (outError) snprintf(outError, errSize, "Sem slots de resultado");
        return -1;
    }
    ResultSlot* res = getResult(resultIdx);
    int64_t tempId = -(resultIdx + 1);   // negative to indicate temporary
    res->id = tempId;
    res->estado = 1;
    res->erro = false;
    res->erroMsg[0] = '\0';

    // Enqueue request
    size_t tail = m_header->queueTail;
    RequestSlot* slot = getQueueSlot(tail % MAX_QUEUE);
    slot->id = tempId;
    strncpy(slot->templateName, templateName, MAX_TEMPLATE_NAME - 1);
    slot->templateName[MAX_TEMPLATE_NAME - 1] = '\0';
    size_t copyLen = dadosLen < MAX_REQUEST_DATA ? dadosLen : MAX_REQUEST_DATA - 1;
    memcpy(slot->dados, dados, copyLen);
    slot->dados[copyLen] = '\0';
    m_header->queueTail++;
    m_header->queueCount++;              // CORRECT

    m_cv.notify_one();

    return tempId;   // temporary ID
}

bool JsonWorker::estaPronto(JsonId id) const {
    if (!m_redis) return false;
    if (id < 0) return false;
    return m_redis->exists(id);
}

bool JsonWorker::pegarJsonPronto(JsonId id, int timeoutMs,
                                 char* outBuffer, size_t bufferSize,
                                 char* outError, size_t errSize) {
    if (!m_redis) {
        if (outError) snprintf(outError, errSize, "FakeRedis não disponível");
        return false;
    }
    if (!outBuffer || bufferSize == 0) {
        if (outError) snprintf(outError, errSize, "Buffer inválido ou tamanho zero");
        return false;
    }

    // Resolve temporary ID if needed
    JsonId realId = id;
    if (id < 0) {
        int slotIdx = static_cast<int>(-id - 1);
        if (slotIdx < 0 || static_cast<size_t>(slotIdx) >= MAX_RESULTS) {
            if (outError) snprintf(outError, errSize, "ID temporário inválido");
            return false;
        }
        const ResultSlot* slot = getResult(slotIdx);
        if (!slot || slot->estado == 0) {
            if (outError) snprintf(outError, errSize, "Slot inválido");
            return false;
        }
        if (slot->estado == 1) {
            // still processing – wait below
        } else if (slot->estado == 2) {
            realId = slot->id;
        } else {
            if (outError) snprintf(outError, errSize, "Slot com erro: %s", slot->erroMsg);
            return false;
        }
    }

    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (timeoutMs > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed >= timeoutMs) {
                if (outError) snprintf(outError, errSize, "Timeout ao aguardar JSON");
                return false;
            }
        }

        if (m_redis->exists(realId)) {
            char errBuf[256] = {0};
            if (m_redis->get(realId, outBuffer, bufferSize, errBuf, sizeof(errBuf))) {
                m_redis->del(realId);

                // Mark slot as delivered
                if (id < 0) {
                    int slotIdx = static_cast<int>(-id - 1);
                    ResultSlot* slot = getResult(slotIdx);
                    if (slot) slot->estado = 3;
                } else {
                    for (size_t i = 0; i < MAX_RESULTS; ++i) {
                        ResultSlot* slot = getResult(static_cast<int>(i));
                        if (slot->estado == 2 && slot->id == realId) {
                            slot->estado = 3;
                            break;
                        }
                    }
                }
                return true;
            } else {
                // Read failed – clean the slot
                if (id < 0) {
                    int slotIdx = static_cast<int>(-id - 1);
                    ResultSlot* slot = getResult(slotIdx);
                    if (slot) { slot->estado = 0; slot->erro = true; }
                }
                if (outError) snprintf(outError, errSize, "Falha ao obter do Redis: %s", errBuf);
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void JsonWorker::shutdown() {
    m_running = false;
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

JsonWorker::Stats JsonWorker::getStats() const {
    Stats stats;
    stats.totalProcessados = m_header->totalProcessados.load();   // CORRECT
    stats.totalErros = m_header->totalErros.load();               // CORRECT
    stats.filaPendente = m_header->queueCount.load();             // CORRECT
    stats.resultadosProntos = 0;
    for (size_t i = 0; i < MAX_RESULTS; ++i) {
        const ResultSlot* slot = getResult(static_cast<int>(i));
        if (slot && slot->estado == 2) stats.resultadosProntos++;
    }
    stats.templatesRegistrados = 0;
    for (size_t i = 0; i < MAX_TEMPLATES; ++i) {
        const TemplateSlot* slot = getTemplate(static_cast<int>(i));
        if (slot && slot->usado) stats.templatesRegistrados++;
    }
    stats.arenaUsadoMB = OFFSET_BUFFER / (1024 * 1024);
    stats.arenaTotalMB = ARENA_SIZE / (1024 * 1024);
    stats.arenaPercentual = (static_cast<float>(OFFSET_BUFFER) / ARENA_SIZE) * 100.0f;
    return stats;
}

} // namespace JSONWorker