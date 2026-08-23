// config.cpp
#include "../include/config.hpp"
#include "../../memorymanager/include/memory_manager_thread.hpp"
#include <iostream>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cerrno>

namespace LerConfig {

// ============================================================
// BUFFER THREAD-LOCAL PARA RETORNO DE STRINGS
// ============================================================
thread_local char Config::return_buffer_[512];

// ============================================================
// ID ÚNICO PARA O CONFIG
// ============================================================
static const uint64_t CONFIG_TID = 0xFFFFFFFF;
static memorymanager::MemoryManagerThread* g_memory_manager = nullptr;

// ============================================================
// FUNÇÃO PARA SETAR O MEMORY MANAGER
// ============================================================

void Config::setMemoryManager(memorymanager::MemoryManagerThread* mm) {
    g_memory_manager = mm;
}

// ============================================================
// SINGLETON
// ============================================================

Config& Config::getInstance() {
    static Config instance;
    return instance;
}

Config::Config() : carregado_(false) {
    std::memset(&storage_, 0, sizeof(storage_));
    std::memset(arquivo_carregado_, 0, sizeof(arquivo_carregado_));
    storage_.entries = nullptr;
    storage_.entry_count = 0;
    storage_.block_start = 0;
    storage_.allocated_blocks = 0;
}

Config::~Config() {
    if (storage_.entries != nullptr && g_memory_manager != nullptr) {
        char error_msg[256] = {0};
        if (!g_memory_manager->free(CONFIG_TID, error_msg)) {
            std::cerr << "❌ LerConfig: Falha ao liberar memória: " << error_msg << std::endl;
        }
        storage_.entries = nullptr;
        storage_.entry_count = 0;
    }
}

// ============================================================
// BUSCA LINEAR
// ============================================================

const ConfigEntry* Config::findEntry(const char* chave) const {
    if (!carregado_ || !storage_.entries) return nullptr;
    for (size_t i = 0; i < storage_.entry_count; i++) {
        if (std::strcmp(storage_.entries[i].key, chave) == 0) {
            return &storage_.entries[i];
        }
    }
    return nullptr;
}

// ============================================================
// CARREGAR - UMA ÚNICA ALOCAÇÃO
// ============================================================

bool Config::carregar(const char* arquivo) {
    // ============================================================
    // 1. VERIFICA SE O MEMORY MANAGER ESTÁ DISPONÍVEL
    // ============================================================
    if (g_memory_manager == nullptr) {
        std::cerr << "❌ LerConfig: MemoryManagerThread não foi setado!" << std::endl;
        return false;
    }

    if (!g_memory_manager->is_running() || !g_memory_manager->is_initialized()) {
        std::cerr << "❌ LerConfig: MemoryManagerThread não está rodando!" << std::endl;
        return false;
    }

    // ============================================================
    // 2. SE JÁ TEM ÍNDICE, LIBERA ANTES DE RECARREGAR
    // ============================================================
    if (storage_.entries != nullptr) {
        char error_msg[256] = {0};
        if (!g_memory_manager->free(CONFIG_TID, error_msg)) {
            std::cerr << "⚠️ LerConfig: Falha ao liberar índice antigo: " << error_msg << std::endl;
        }
        storage_.entries = nullptr;
        storage_.entry_count = 0;
    }

    // ============================================================
    // 3. ABRE ARQUIVO
    // ============================================================
    FILE* fp = fopen(arquivo, "r");
    if (!fp) {
        std::cerr << "❌ LerConfig: Não foi possível abrir " << arquivo << " (" << strerror(errno) << ")" << std::endl;
        return false;
    }

    // ============================================================
    // 4. PRIMEIRA PASSADA: CONTA LINHAS VÁLIDAS
    // ⚠️ USO CONTROLADO DE STACK: buffer line[512] (temporário)
    // ============================================================
    char line[512];  // ← ÚNICO buffer na stack! Pequeno e temporário.
    size_t line_count = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        // Remove newline
        size_t len = std::strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (len > 0 && line[len-1] == '\r') line[len-1] = '\0';

        // Pula espaços iniciais
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;

        // Procura por '='
        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        if (eq == p) continue;

        line_count++;
    }

    if (line_count == 0) {
        std::cerr << "❌ LerConfig: Arquivo vazio ou sem chaves válidas." << std::endl;
        fclose(fp);
        return false;
    }

    // ============================================================
    // 5. ALOCA ÍNDICE NO MEMORY MANAGER (ÚNICA ALOCAÇÃO!)
    // ============================================================
    size_t index_size = line_count * sizeof(ConfigEntry);
    size_t block_size = 1024ULL * 1024ULL; // 1 MB
    uint32_t blocks_needed = static_cast<uint32_t>((index_size + block_size - 1) / block_size);
    if (blocks_needed == 0u) blocks_needed = 1u;

    uint32_t start_block = 0u;
    void* start_addr = nullptr;
    void* end_addr = nullptr;
    char error_msg[256] = {0};

    if (!g_memory_manager->allocate(CONFIG_TID, blocks_needed, start_block,
                                     start_addr, end_addr, error_msg)) {
        std::cerr << "❌ LerConfig: Falha ao alocar índice: " << error_msg << std::endl;
        fclose(fp);
        return false;
    }

    if (start_addr == nullptr) {
        std::cerr << "❌ LerConfig: Índice alocado é nulo!" << std::endl;
        g_memory_manager->free(CONFIG_TID, error_msg);
        fclose(fp);
        return false;
    }

    ConfigEntry* entries = static_cast<ConfigEntry*>(start_addr);

    // ============================================================
    // 6. SEGUNDA PASSADA: VOLTA AO INÍCIO E PREENCHE O ÍNDICE
    // ============================================================
    rewind(fp);
    size_t entry_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = std::strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (len > 0 && line[len-1] == '\r') line[len-1] = '\0';

        // Remove BOM (UTF-8) se presente
        if (std::strncmp(line, "\xEF\xBB\xBF", 3) == 0) {
            std::memmove(line, line + 3, std::strlen(line + 3) + 1);
        }

        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;

        char* eq = std::strchr(p, '=');
        if (!eq) continue;
        if (eq == p) continue;

        // Extrai chave
        char* key_start = p;
        char* key_end = eq - 1;
        while (key_end > key_start && (*key_end == ' ' || *key_end == '\t')) key_end--;
        if (key_end <= key_start) continue;
        size_t key_len = key_end - key_start + 1;
        if (key_len >= 64) continue;

        // Extrai valor
        char* val_start = eq + 1;
        while (*val_start == ' ' || *val_start == '\t') val_start++;
        char* val_end = val_start + std::strlen(val_start) - 1;
        while (val_end > val_start && (*val_end == ' ' || *val_end == '\t' || *val_end == '\r' || *val_end == '\n')) val_end--;
        if (val_end < val_start) continue;
        size_t val_len = val_end - val_start + 1;
        if (val_len >= 256) continue;

        // Remove aspas se houver
        if (val_len >= 2 && val_start[0] == '"' && val_start[val_len-1] == '"') {
            val_start++;
            val_len -= 2;
        }

        // ============================================================
        // COPIA PARA O ÍNDICE (MEMORY MANAGER)
        // ============================================================
        ConfigEntry* entry = &entries[entry_count];
        std::memcpy(entry->key, key_start, key_len);
        entry->key[key_len] = '\0';
        std::memcpy(entry->value, val_start, val_len);
        entry->value[val_len] = '\0';
        entry_count++;
    }

    fclose(fp);

    // ============================================================
    // 7. ATUALIZA STORAGE
    // ============================================================
    storage_.entries = entries;
    storage_.entry_count = entry_count;
    storage_.block_start = start_block;
    storage_.allocated_blocks = blocks_needed;

    size_t arquivo_len = std::strlen(arquivo);
    if (arquivo_len < sizeof(arquivo_carregado_)) {
        std::memcpy(arquivo_carregado_, arquivo, arquivo_len + 1);
    }

    carregado_ = true;

    std::cout << "📖 LerConfig: " << entry_count
              << " configurações carregadas de " << arquivo
              << " (" << blocks_needed << " blocos alocados)"
              << std::endl;
    return true;
}

// ============================================================
// RECARREGAR
// ============================================================

bool Config::recarregar() {
    if (arquivo_carregado_[0] == '\0') {
        std::cerr << "❌ LerConfig: Nenhum arquivo previamente carregado." << std::endl;
        return false;
    }
    return carregar(arquivo_carregado_);
}

// ============================================================
// GETTERS
// ============================================================

const char* Config::getString(const char* chave, const char* padrao) const {
    const ConfigEntry* entry = findEntry(chave);
    if (entry) {
        size_t len = std::strlen(entry->value);
        if (len >= sizeof(return_buffer_)) len = sizeof(return_buffer_) - 1;
        std::memcpy(return_buffer_, entry->value, len);
        return_buffer_[len] = '\0';
        return return_buffer_;
    }
    return padrao;
}

int Config::getInt(const char* chave, int padrao) const {
    const ConfigEntry* entry = findEntry(chave);
    if (entry) {
        char* endptr;
        long valor = std::strtol(entry->value, &endptr, 10);
        if (endptr != entry->value) return static_cast<int>(valor);
    }
    return padrao;
}

size_t Config::getSize(const char* chave, size_t padrao) const {
    const ConfigEntry* entry = findEntry(chave);
    if (entry) {
        char* endptr;
        unsigned long valor = std::strtoul(entry->value, &endptr, 10);
        if (endptr != entry->value) return static_cast<size_t>(valor);
    }
    return padrao;
}

float Config::getFloat(const char* chave, float padrao) const {
    const ConfigEntry* entry = findEntry(chave);
    if (entry) {
        char* endptr;
        float valor = std::strtof(entry->value, &endptr);
        if (endptr != entry->value) return valor;
    }
    return padrao;
}

bool Config::getBool(const char* chave, bool padrao) const {
    const ConfigEntry* entry = findEntry(chave);
    if (!entry) return padrao;

    auto cmp = [](const char* a, const char* b) -> bool {
        while (*a && *b) {
            if (std::tolower(*a) != std::tolower(*b)) return false;
            a++; b++;
        }
        return *a == '\0' && *b == '\0';
    };

    if (cmp(entry->value, "true") || cmp(entry->value, "1") ||
        cmp(entry->value, "yes") || cmp(entry->value, "on")) {
        return true;
    }
    if (cmp(entry->value, "false") || cmp(entry->value, "0") ||
        cmp(entry->value, "no") || cmp(entry->value, "off")) {
        return false;
    }
    return padrao;
}

bool Config::existe(const char* chave) const {
    return findEntry(chave) != nullptr;
}

void Config::listarTodas() const {
    if (!carregado_ || !storage_.entries) {
        std::cout << "📋 Nenhuma configuração carregada." << std::endl;
        return;
    }
    std::cout << "\n📋 CONFIGURAÇÕES CARREGADAS (Zero-Heap):" << std::endl;
    std::cout << "================================" << std::endl;
    for (size_t i = 0; i < storage_.entry_count; i++) {
        if (std::strcmp(storage_.entries[i].key, "db_password") == 0) {
            std::cout << "  " << storage_.entries[i].key << " = ********" << std::endl;
        } else {
            std::cout << "  " << storage_.entries[i].key << " = "
                      << storage_.entries[i].value << std::endl;
        }
    }
    std::cout << "================================" << std::endl;
}

} // namespace LerConfig