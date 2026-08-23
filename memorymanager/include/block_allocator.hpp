// block_allocator.hpp
#ifndef BLOCK_ALLOCATOR_HPP
#define BLOCK_ALLOCATOR_HPP

#include <cstdint>
#include <cstddef>
#include <array>
#include <mutex>

namespace memorymanager {

// ============================================================
// CONFIGURAÇÕES MÁXIMAS (AJUSTÁVEIS CONFORME NECESSIDADE)
// ============================================================

/**
 * @brief Número máximo de blocos suportados.
 * 
 * Cálculo: 30GB / 1MB = 30720 blocos
 * Arredondamos para 32768 (potência de 2) para facilitar alinhamento.
 * 
 * ⚠️ SE PRECISAR DE MAIS: ajuste esta constante!
 *    - Para 60GB: MAX_BLOCKS = 65536
 *    - Para 120GB: MAX_BLOCKS = 131072
 */
constexpr size_t MAX_BLOCKS = 32768;          // 30GB com blocos de 1MB
constexpr size_t MAX_BITMAP_BYTES = (MAX_BLOCKS + 7u) / 8u;  // 4096 bytes
constexpr size_t MAX_LOANS = 1024;            // Máximo de threads simultâneas

// ============================================================
// ESTRUTURAS DE DADOS
// ============================================================

struct LoanEntry {
    uint64_t id{0u};
    uint32_t start_block{0u};
    uint32_t num_blocks{0u};
};

// ============================================================
// EXCEÇÃO À REGRA: BITMAP NA STACK
// ============================================================

/**
 * @brief EXCEÇÃO JUSTIFICADA: Por que o bitmap está na stack?
 * 
 * NORMATIVA GERAL: "Prioridade Absoluta à Arena (heap)"
 * 
 * DESVIO JUSTIFICADO:
 * 
 * 1. TAMANHO FIXO E PREDIZÍVEL
 *    - O bitmap NUNCA muda de tamanho após a inicialização
 *    - MAX_BLOCKS é conhecido em tempo de compilação
 *    - MAX_BITMAP_BYTES = 4096 bytes (constante)
 * 
 * 2. ZERO ALOCAÇÕES DINÂMICAS NO HEAP
 *    - A região mmap fica 100% dedicada a dados do usuário
 *    - Sem overhead de gerenciamento de heap
 *    - Sem risco de fragmentação
 * 
 * 3. PERFORMANCE SUPERIOR
 *    - Acesso à stack é mais rápido que heap
 *    - Sem custo de alocação/desalocação
 *    - Cache-friendly (dados contíguos)
 * 
 * 4. SEGURANÇA
 *    - Não pode falhar por falta de heap
 *    - Stack é isolada por thread (com mutex para compartilhamento)
 * 
 * 5. ESPAÇO NA STACK É PEQUENO
 *    - BITMAP: 4KB (MAX_BITMAP_BYTES)
 *    - LOAN_TABLE: 24KB (1024 * 24 bytes)
 *    - TOTAL: ~28KB (bem abaixo do limite típico de 8MB)
 * 
 * ⚠️ ATENÇÃO: Esta é uma EXCEÇÃO CONTROLADA à regra geral
 *    Aprovada porque o benefício supera o custo
 *    e o tamanho é conhecido e pequeno.
 */
struct BlockAllocator {
    // 📦 Dados na STACK (tamanho fixo e conhecido)
    std::array<uint8_t, MAX_BITMAP_BYTES> bitmap{};
    std::array<LoanEntry, MAX_LOANS> loan_table{};
    
    // Ponteiros (apontam para os arrays acima)
    uint8_t* bitmap_ptr{nullptr};
    LoanEntry* loan_table_ptr{nullptr};
    
    // Metadados
    uint32_t total_blocks{0u};
    size_t block_size{0u};
    uint32_t max_loans{MAX_LOANS};
    uint32_t active_loans{0u};
    void* data_start{nullptr};
    
    // 🔒 Mutex para acesso exclusivo entre threads
    mutable std::mutex mtx;
    
    // ============================================================
    // CONSTRUTOR
    // ============================================================
    
    /**
     * @brief Inicializa todas as estruturas com zero.
     * 
     * O bitmap e a loan_table são zerados completamente,
     * garantindo estado inicial consistente.
     */
    BlockAllocator() 
        : bitmap_ptr(bitmap.data())
        , loan_table_ptr(loan_table.data()) {
        // Zera todo o bitmap (4KB)
        bitmap.fill(0u);
        
        // Zera toda a tabela de empréstimos (24KB)
        loan_table.fill(LoanEntry{0u, 0u, 0u});
    }
    
    // Desabilitar cópia (mutex não é copiável)
    BlockAllocator(const BlockAllocator&) = delete;
    BlockAllocator& operator=(const BlockAllocator&) = delete;
};

// ============================================================
// FUNÇÕES PÚBLICAS
// ============================================================

/**
 * @brief Inicializa o BlockAllocator com uma região mmap.
 * 
 * @param base_addr   Ponteiro para a região mmap (NUNCA é nullptr)
 * @param total_size  Tamanho total da região mmap
 * @param alloc       Estrutura BlockAllocator a ser inicializada
 * @param usable_start [saída] Início da área utilizável (void*)
 * @param usable_size  [saída] Tamanho da área utilizável
 * @return true em sucesso, false em erro
 * 
 * @note O bitmap e loan_table estão na STACK (não na região mmap)
 *       Isso libera TODO o espaço mmap para dados do usuário.
 */
bool init_block_allocator(void* base_addr, size_t total_size,
                          BlockAllocator& alloc,
                          uint64_t& usable_start, size_t& usable_size);

/**
 * @brief Aloca blocos contíguos para uma thread.
 * 
 * @param alloc      Estrutura BlockAllocator
 * @param thread_id  ID da thread solicitante
 * @param num_blocks Número de blocos a alocar
 * @return int64_t   Índice do primeiro bloco ou código de erro:
 *                   - ≥ 0: sucesso (índice do bloco)
 *                   - -1: sem espaço contíguo
 *                   - -2: thread já possui empréstimo
 * 
 * @note Thread-safe: usa mutex para acesso exclusivo ao bitmap
 */
int64_t allocate_blocks(BlockAllocator& alloc, 
                        uint64_t thread_id, 
                        uint32_t num_blocks);

/**
 * @brief Libera todos os blocos alocados por uma thread.
 * 
 * @param alloc      Estrutura BlockAllocator
 * @param thread_id  ID da thread que vai liberar
 * @return true em sucesso, false se thread não encontrada
 * 
 * @note Thread-safe: usa mutex para acesso exclusivo ao bitmap
 */
bool free_blocks_by_id(BlockAllocator& alloc, 
                       uint64_t thread_id);

/**
 * @brief Verifica se um bloco específico está alocado.
 * 
 * @param alloc       Estrutura BlockAllocator
 * @param block_index Índice do bloco a verificar
 * @return true se alocado, false se livre
 * 
 * @note Thread-safe: usa mutex para leitura consistente
 */
bool is_block_allocated(const BlockAllocator& alloc, 
                        uint32_t block_index);

/**
 * @brief Obtém o número de empréstimos ativos.
 * 
 * @param alloc Estrutura BlockAllocator
 * @return uint32_t Número de threads com empréstimo ativo
 * 
 * @note Thread-safe: usa mutex para leitura consistente
 */
uint32_t get_active_loans(const BlockAllocator& alloc);

/**
 * @brief Obtém o número total de blocos.
 * 
 * @param alloc Estrutura BlockAllocator
 * @return uint32_t Número total de blocos
 * 
 * @note Thread-safe: usa mutex para leitura consistente
 */
inline uint32_t get_total_blocks(const BlockAllocator& alloc) {
    std::lock_guard<std::mutex> lock(alloc.mtx);
    return alloc.total_blocks;
}

/**
 * @brief Obtém o tamanho do bloco.
 * 
 * @param alloc Estrutura BlockAllocator
 * @return size_t Tamanho do bloco em bytes
 * 
 * @note Thread-safe: usa mutex para leitura consistente
 */
inline size_t get_block_size(const BlockAllocator& alloc) {
    std::lock_guard<std::mutex> lock(alloc.mtx);
    return alloc.block_size;
}

} // namespace memorymanager

#endif // BLOCK_ALLOCATOR_HPP