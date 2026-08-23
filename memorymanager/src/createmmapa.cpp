// createmmapa.cpp
#include "../include/createmmapa.hpp"
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>
#include <cstring> // para std::memset

namespace memorymanager {

// ============================================================
// CONSTANTES (JÁ DEFINIDAS NO HEADER - NÃO REDEFINIR!)
// ============================================================

// As constantes MIN_REGION_SIZE e MAX_REGION_SIZE já estão
// definidas no arquivo .hpp. Não as redefine aqui!

// ============================================================
// ESTADO GLOBAL DA REGIÃO (Thread-Safe)
// ============================================================

static std::atomic<bool> g_region_allocated{false};
static void* g_region_base{nullptr};
static size_t g_region_size{0u};

// ============================================================
// TAMANHO DA PÁGINA (para touch eficiente)
// ============================================================

static constexpr size_t PAGE_SIZE = 4096ULL; // 4KB

// ============================================================
// FUNÇÕES AUXILIARES (PRIVADAS)
// ============================================================

/**
 * @brief Touch all pages in a memory region to force physical allocation.
 * 
 * O mmap com MAP_ANONYMOUS aloca memória virtual, mas as páginas
 * físicas só são alocadas quando acessadas (demand paging).
 * 
 * Esta função escreve um byte em cada página para forçar a alocação
 * física e garantir que a memória realmente existe.
 * 
 * @param ptr   Ponteiro para o início da região
 * @param size  Tamanho da região em bytes
 * @return true se todas as páginas foram tocadas com sucesso
 * 
 * @note Esta operação pode ser lenta para regiões grandes (30GB)
 * @note Garante que a memória está realmente disponível
 * @note Usa std::memset para tocar todas as páginas de uma vez
 */
static bool touch_memory_pages(void* ptr, size_t size) {
    if (ptr == nullptr || size == 0u) {
        return false;
    }

    // Usa memset para escrever em toda a região
    // Isso força o sistema operacional a alocar páginas físicas
    // para todas as páginas virtuais da região
    std::memset(ptr, 0, size);
    
    // Verifica se a operação foi bem sucedida
    // Em caso de erro, o sistema operacional enviaria um sinal SIGSEGV
    // Como chegamos aqui, a memória está alocada
    return true;
}

// ============================================================
// FUNÇÕES PÚBLICAS
// ============================================================

bool allocate_region(size_t requested_size, void*& base, size_t& total_size) {
    // ============================================================
    // VALIDAÇÕES IMEDIATAS
    // ============================================================
    
    // Verifica se já existe uma região alocada
    if (g_region_allocated.load()) {
        return false;
    }
    
    // Verifica tamanho mínimo (1GB)
    if (requested_size < MIN_REGION_SIZE) {
        return false;
    }
    
    // Verifica tamanho máximo (30GB)
    if (requested_size > MAX_REGION_SIZE) {
        return false;
    }

    // ============================================================
    // CHAMADA DE SISTEMA (mmap)
    // ============================================================
    
    // Tenta alocar a região com mmap
    void* mapped = mmap(nullptr, requested_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    
    // Verifica se mmap falhou
    if (mapped == MAP_FAILED) {
        return false;
    }

    // ============================================================
    // TOUCH DA MEMÓRIA (Forçar Alocação Física)
    // ============================================================
    
    // IMPORTANTE: O mmap com MAP_ANONYMOUS aloca memória virtual,
    // mas as páginas físicas só são alocadas quando acessadas.
    // 
    // Precisamos "tocar" (escrever em) cada página para garantir
    // que o sistema operacional realmente aloque a memória física.
    // 
    // Usamos memset para tocar todas as páginas de forma eficiente.
    // O compilador otimiza isso para operações de bloco.
    if (!touch_memory_pages(mapped, requested_size)) {
        // Se falhar ao tocar a memória, libera a região
        munmap(mapped, requested_size);
        return false;
    }

    // ============================================================
    // ATUALIZAÇÃO DO ESTADO GLOBAL (Thread-Safe)
    // ============================================================
    
    // Atualiza as variáveis globais atomicamente
    g_region_base = mapped;
    g_region_size = requested_size;
    g_region_allocated.store(true);

    // ============================================================
    // PREENCHIMENTO DOS PARÂMETROS DE SAÍDA
    // ============================================================
    
    base = mapped;
    total_size = requested_size;
    return true;
}

bool free_region(void* base, size_t total_size) {
    // ============================================================
    // VALIDAÇÕES IMEDIATAS
    // ============================================================
    
    // Verifica se há uma região alocada
    if (!g_region_allocated.load()) {
        return false;
    }
    
    // Verifica se os parâmetros são válidos
    if (base == nullptr) {
        return false;
    }
    if (total_size == 0u) {
        return false;
    }
    
    // Verifica se o ponteiro e tamanho correspondem à região alocada
    if (base != g_region_base) {
        return false;
    }
    if (total_size != g_region_size) {
        return false;
    }

    // ============================================================
    // CHAMADA DE SISTEMA (munmap)
    // ============================================================
    
    // Tenta liberar a região com munmap
    if (munmap(base, total_size) != 0) {
        return false;
    }

    // ============================================================
    // LIMPEZA DO ESTADO GLOBAL (Thread-Safe)
    // ============================================================
    
    // Limpa as variáveis globais atomicamente
    g_region_allocated.store(false);
    g_region_base = nullptr;
    g_region_size = 0u;
    return true;
}

bool is_region_allocated() {
    return g_region_allocated.load();
}

bool get_region_info(void*& base, size_t& size) {
    if (!g_region_allocated.load()) {
        base = nullptr;
        size = 0u;
        return false;
    }
    
    base = g_region_base;
    size = g_region_size;
    return true;
}

} // namespace memorymanager