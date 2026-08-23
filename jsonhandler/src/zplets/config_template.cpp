#include "../include/config_template.hpp"
#include "../../uteis/include/uteis.hpp"   // para safeCopyString
#include <cstring>
#include <cstdio>
#include <mutex>

namespace JSONWorker {

static std::mutex g_templateMutex;

static const char* DEFAULT_TEMPLATES[] = {
    "template_01", "{\"message\":\"Hello world 01\"}",
    "template_02", "{\"message\":\"Hello world 02\"}",
    "template_03", "{\"message\":\"Hello world 03\"}",
    "template_04", "{\"message\":\"Hello world 04\"}",
    "template_05", "{\"message\":\"Hello world 05\"}",
    "template_06", "{\"message\":\"Hello world 06\"}",
    "template_07", "{\"message\":\"Hello world 07\"}",
    "template_08", "{\"message\":\"Hello world 08\"}",
    "template_09", "{\"message\":\"Hello world 09\"}",
    "template_10", "{\"message\":\"Hello world 10\"}"
};
static const size_t NUM_DEFAULT_TEMPLATES = sizeof(DEFAULT_TEMPLATES) / (2 * sizeof(const char*));

static TemplateEntry* find_entry(TemplateEntry* entries, uint32_t count, const char* nome) {
    for (uint32_t i = 0; i < count; ++i) {
        if (strncmp(entries[i].nome, nome, 13) == 0) {
            return &entries[i];
        }
    }
    return nullptr;
}

static size_t calc_free_offset(const TemplateEntry* entries, uint32_t count) {
    if (count == 0) {
        return sizeof(TemplateHeader) + MAX_TEMPLATES * sizeof(TemplateEntry);
    }
    size_t maxOffset = 0;
    for (uint32_t i = 0; i < count; ++i) {
        size_t end = entries[i].offset + entries[i].tamanho + 1;
        if (end > maxOffset) maxOffset = end;
    }
    maxOffset = (maxOffset + 3) & ~3;
    return maxOffset;
}

bool start_template(void* memory, size_t memorySize, char* outError, size_t errSize) {
    if (!memory || memorySize < 4096) {
        Utils::safeCopyString(outError, errSize, "Invalid memory or too small");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_templateMutex);

    uint8_t* base = static_cast<uint8_t*>(memory);
    TemplateHeader* header = reinterpret_cast<TemplateHeader*>(base);
    header->magic = MAGIC_START;
    header->count = 0;

    TemplateEntry* entries = reinterpret_cast<TemplateEntry*>(base + sizeof(TemplateHeader));
    size_t contentOffset = sizeof(TemplateHeader) + MAX_TEMPLATES * sizeof(TemplateEntry);

    for (size_t i = 0; i < NUM_DEFAULT_TEMPLATES && i < MAX_TEMPLATES; ++i) {
        const char* nome = DEFAULT_TEMPLATES[i * 2];
        const char* conteudo = DEFAULT_TEMPLATES[i * 2 + 1];
        size_t conteudoLen = strlen(conteudo);

        if (contentOffset + conteudoLen + 1 > memorySize) {
            Utils::safeCopyString(outError, errSize, "Insufficient memory for default templates");
            return false;
        }

        strncpy(entries[i].nome, nome, 13);
        entries[i].nome[13] = '\0';
        entries[i].offset = static_cast<uint32_t>(contentOffset);
        entries[i].tamanho = static_cast<uint32_t>(conteudoLen);

        uint8_t* dest = base + contentOffset;
        memcpy(dest, conteudo, conteudoLen);
        dest[conteudoLen] = '\0';

        contentOffset += conteudoLen + 1;
        contentOffset = (contentOffset + 3) & ~3;
    }

    header->count = static_cast<uint32_t>(NUM_DEFAULT_TEMPLATES);
    uint32_t* magicEnd = reinterpret_cast<uint32_t*>(base + contentOffset);
    *magicEnd = MAGIC_END;

    return true;
}

bool consultar_template(const void* memory, size_t memorySize,
                        const char* nome,
                        char* outBuffer, size_t bufferSize,
                        char* outError, size_t errSize) {
    (void)memorySize;
    if (!memory || !nome || !outBuffer) {
        Utils::safeCopyString(outError, errSize, "Invalid parameters");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_templateMutex);

    const uint8_t* base = static_cast<const uint8_t*>(memory);
    const TemplateHeader* header = reinterpret_cast<const TemplateHeader*>(base);
    if (header->magic != MAGIC_START) {
        Utils::safeCopyString(outError, errSize, "Invalid magic (corrupted memory)");
        return false;
    }

    const TemplateEntry* entries = reinterpret_cast<const TemplateEntry*>(base + sizeof(TemplateHeader));
    const TemplateEntry* entry = find_entry(const_cast<TemplateEntry*>(entries), header->count, nome);
    if (!entry) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Template '%s' not found", nome);
        Utils::safeCopyString(outError, errSize, msg);
        return false;
    }

    if (entry->offset + entry->tamanho > memorySize) {
        Utils::safeCopyString(outError, errSize, "Corrupted data (invalid offset)");
        return false;
    }

    if (entry->tamanho + 1 > bufferSize) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Output buffer too small (requires %u bytes)", entry->tamanho + 1);
        Utils::safeCopyString(outError, errSize, msg);
        return false;
    }

    const char* src = reinterpret_cast<const char*>(base + entry->offset);
    memcpy(outBuffer, src, entry->tamanho);
    outBuffer[entry->tamanho] = '\0';

    return true;
}

bool gravar_template(void* memory, size_t memorySize,
                     const char* nome, const char* conteudo,
                     char* outError, size_t errSize) {
    (void)memorySize;
    if (!memory || !nome || !conteudo) {
        Utils::safeCopyString(outError, errSize, "Invalid parameters");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_templateMutex);

    uint8_t* base = static_cast<uint8_t*>(memory);
    TemplateHeader* header = reinterpret_cast<TemplateHeader*>(base);
    if (header->magic != MAGIC_START) {
        Utils::safeCopyString(outError, errSize, "Invalid magic (corrupted memory)");
        return false;
    }

    TemplateEntry* entries = reinterpret_cast<TemplateEntry*>(base + sizeof(TemplateHeader));
    TemplateEntry* entry = find_entry(entries, header->count, nome);

    size_t nomeLen = strlen(nome);
    size_t conteudoLen = strlen(conteudo);
    if (nomeLen >= 14) {
        Utils::safeCopyString(outError, errSize, "Name too long (max 13 chars)");
        return false;
    }

    if (entry) {
        if (conteudoLen <= entry->tamanho) {
            uint8_t* dest = base + entry->offset;
            memcpy(dest, conteudo, conteudoLen);
            dest[conteudoLen] = '\0';
            entry->tamanho = static_cast<uint32_t>(conteudoLen);
            return true;
        } else {
            if (header->count >= MAX_TEMPLATES) {
                Utils::safeCopyString(outError, errSize, "Maximum template limit reached");
                return false;
            }
            entry->nome[0] = '\0';
            TemplateEntry* newEntry = &entries[header->count];
            strncpy(newEntry->nome, nome, 13);
            newEntry->nome[13] = '\0';
            size_t newOffset = calc_free_offset(entries, header->count);
            if (newOffset + conteudoLen + 1 > memorySize) {
                Utils::safeCopyString(outError, errSize, "Insufficient memory");
                return false;
            }
            newEntry->offset = static_cast<uint32_t>(newOffset);
            newEntry->tamanho = static_cast<uint32_t>(conteudoLen);
            uint8_t* dest = base + newOffset;
            memcpy(dest, conteudo, conteudoLen);
            dest[conteudoLen] = '\0';
            header->count++;
            return true;
        }
    }

    if (header->count >= MAX_TEMPLATES) {
        Utils::safeCopyString(outError, errSize, "Maximum template limit reached");
        return false;
    }

    size_t newOffset = calc_free_offset(entries, header->count);
    if (newOffset + conteudoLen + 1 > memorySize) {
        Utils::safeCopyString(outError, errSize, "Insufficient memory");
        return false;
    }

    TemplateEntry* newEntry = &entries[header->count];
    strncpy(newEntry->nome, nome, 13);
    newEntry->nome[13] = '\0';
    newEntry->offset = static_cast<uint32_t>(newOffset);
    newEntry->tamanho = static_cast<uint32_t>(conteudoLen);

    uint8_t* dest = base + newOffset;
    memcpy(dest, conteudo, conteudoLen);
    dest[conteudoLen] = '\0';

    header->count++;
    return true;
}

bool remover_template(void* memory, size_t memorySize,
                      const char* nome,
                      char* outError, size_t errSize) {
    (void)memorySize;
    if (!memory || !nome) {
        Utils::safeCopyString(outError, errSize, "Invalid parameters");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_templateMutex);

    uint8_t* base = static_cast<uint8_t*>(memory);
    TemplateHeader* header = reinterpret_cast<TemplateHeader*>(base);
    if (header->magic != MAGIC_START) {
        Utils::safeCopyString(outError, errSize, "Invalid magic (corrupted memory)");
        return false;
    }

    TemplateEntry* entries = reinterpret_cast<TemplateEntry*>(base + sizeof(TemplateHeader));
    TemplateEntry* entry = find_entry(entries, header->count, nome);
    if (!entry) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Template '%s' not found", nome);
        Utils::safeCopyString(outError, errSize, msg);
        return false;
    }

    entry->nome[0] = '\0';
    return true;
}

bool verify_template_memory(const void* memory, size_t memorySize) {
    if (!memory || memorySize < 8) return false;
    const uint8_t* base = static_cast<const uint8_t*>(memory);

    const TemplateHeader* header = reinterpret_cast<const TemplateHeader*>(base);
    if (header->magic != MAGIC_START) return false;
    if (header->count > MAX_TEMPLATES) return false;

    const TemplateEntry* entries = reinterpret_cast<const TemplateEntry*>(base + sizeof(TemplateHeader));
    for (uint32_t i = 0; i < header->count; ++i) {
        if (entries[i].offset + entries[i].tamanho > memorySize) return false;
    }

    const uint32_t* endMagic = reinterpret_cast<const uint32_t*>(base + memorySize - 4);
    if (*endMagic != MAGIC_END) return false;

    return true;
}

} // namespace JSONWorker