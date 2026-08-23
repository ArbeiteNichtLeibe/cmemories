// config.hpp
#ifndef LERCONFIG_HPP
#define LERCONFIG_HPP

#include <cstddef>
#include <cstdint>
#include "../../memorymanager/include/memory_manager_thread.hpp"  // ← HEADER COMPLETO

namespace LerConfig {

// ============================================================
// ESTRUTURA DE ENTRADA
// ============================================================

struct ConfigEntry {
    char key[64];
    char value[256];
};

// ============================================================
// ESTRUTURA DE STORAGE (GERENCIADA PELO MEMORY MANAGER)
// ============================================================

struct ConfigStorage {
    ConfigEntry* entries;
    size_t entry_count;
    uint32_t block_start;
    uint32_t allocated_blocks;
};

// ============================================================
// CLASSE CONFIG (SINGLETON)
// ============================================================

class Config {
public:
    static Config& getInstance();

    // ============================================================
    // INJEÇÃO DO MEMORY MANAGER
    // ============================================================
    static void setMemoryManager(memorymanager::MemoryManagerThread* mm);

    // ============================================================
    // CARREGAR / RECARREGAR
    // ============================================================
    bool carregar(const char* arquivo);
    bool recarregar();

    // ============================================================
    // GETTERS (THREAD-LOCAL BUFFER)
    // ============================================================
    const char* getString(const char* chave, const char* padrao = "") const;
    int getInt(const char* chave, int padrao = 0) const;
    size_t getSize(const char* chave, size_t padrao = 0) const;
    float getFloat(const char* chave, float padrao = 0.0f) const;
    bool getBool(const char* chave, bool padrao = false) const;
    bool existe(const char* chave) const;
    void listarTodas() const;

private:
    Config();
    ~Config();

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;

    const ConfigEntry* findEntry(const char* chave) const;

    ConfigStorage storage_;
    bool carregado_;
    char arquivo_carregado_[256];

    static thread_local char return_buffer_[512];
};

} // namespace LerConfig

#endif // LERCONFIG_HPP