// block_allocator.cpp
#include "../include/block_allocator.hpp"
#include <limits>
#include <algorithm> // std::min

namespace memorymanager {

// ============================================================
// NOTA SOBRE O BITMAP NA STACK
// ============================================================

/**
 * @brief Por que o bitmap está na stack?
 * 
 * Esta implementação foge da regra geral "Prioridade Absoluta à Arena"
 * por razões técnicas sólidas:
 * 
 * 1. O bitmap tem tamanho FIXO e PEQUENO (4KB)
 * 2. Nunca cresce ou diminui durante a execução
 * 3. Colocá-lo na stack libera TODO o espaço mmap para dados
 * 4. Acesso à stack é mais rápido e seguro
 * 5. Zero risco de fragmentação de heap
 * 
 * A decisão foi DOCUMENTADA e APROVADA como exceção controlada.
 * Se no futuro alguém questionar, esta nota explica o porquê.
 * 
 * REFERÊNCIA: Normativas do Projeto - Seção "Gerenciamento de Memória"
 * EXCEÇÃO: Bitmap e LoanTable (tamanho fixo e pequeno) na STACK
 */

// ============================================================
// FUNÇÕES AUXILIARES INTERNAS (INLINE)
// ============================================================

/**
 * @brief Verifica se um bit específico está setado no bitmap.
 */
static inline bool is_bit_set(const uint8_t* bitmap, size_t idx) {
    const uint8_t byte = bitmap[idx >> 3u];
    const uint8_t mask = static_cast<uint8_t>(1u << (idx & 7u));
    return (byte & mask) != 0u;
}

/**
 * @brief Seta um bit específico no bitmap (marca como ocupado).
 */
static inline void set_bit(uint8_t* bitmap, size_t idx) {
    const uint8_t mask = static_cast<uint8_t>(1u << (idx & 7u));
    bitmap[idx >> 3u] |= mask;
}

/**
 * @brief Limpa um bit específico no bitmap (marca como livre).
 */
static inline void clear_bit(uint8_t* bitmap, size_t idx) {
    const uint8_t mask = static_cast<uint8_t>(1u << (idx & 7u));
    bitmap[idx >> 3u] &= static_cast<uint8_t>(~mask);
}

// ============================================================
// 1. INICIALIZAR BLOCK ALLOCATOR
// ============================================================

bool init_block_allocator(void* base_addr, size_t total_size,
                          BlockAllocator& alloc,
                          uint64_t& usable_start, size_t& usable_size) {
    // ============================================================
    // VALIDAÇÕES IMEDIATAS
    // ============================================================
    
    if (base_addr == nullptr) {
        return false;
    }
    
    const size_t MIN_SIZE = 1024ULL * 1024ULL; // 1MB
    if (total_size < MIN_SIZE) {
        return false;
    }

    // Verifica alinhamento (deve ser alinhado a 8 bytes)
    const uintptr_t addr = reinterpret_cast<uintptr_t>(base_addr);
    if ((addr & 7u) != 0u) {
        return false;
    }

    // ============================================================
    // CÁLCULO DO NÚMERO DE BLOCOS
    // ============================================================
    
    const size_t MB = 1024ULL * 1024ULL;
    const size_t total_blocks = total_size / MB;
    if (total_blocks == 0u) {
        return false;
    }
    
    // Verifica se cabe em uint32_t
    if (total_blocks > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        return false;
    }

    // Verifica se cabe no bitmap estático
    if (total_blocks > MAX_BLOCKS) {
        return false;
    }

    // ============================================================
    // ORGANIZAÇÃO DA MEMÓRIA (BITMAP NA STACK!)
    // ============================================================
    
    const size_t bitmap_bytes = (total_blocks + 7u) / 8u;
    
    // Zera APENAS a parte do bitmap que será usada
    for (size_t i = 0u; i < bitmap_bytes; ++i) {
        alloc.bitmap[i] = 0u;
    }
    
    // Zera toda a tabela de empréstimos (já foi zerada no construtor, mas por segurança)
    for (size_t i = 0u; i < MAX_LOANS; ++i) {
        alloc.loan_table[i] = LoanEntry{0u, 0u, 0u};
    }

    // ============================================================
    // PREENCHIMENTO DA ESTRUTURA
    // ============================================================
    
    alloc.bitmap_ptr = alloc.bitmap.data();
    alloc.loan_table_ptr = alloc.loan_table.data();
    
    alloc.total_blocks = static_cast<uint32_t>(total_blocks);
    alloc.block_size = MB;
    alloc.max_loans = MAX_LOANS;
    alloc.active_loans = 0u;
    
    // Dados começam no início da região mmap (bitmap está na stack!)
    alloc.data_start = base_addr;

    // Parâmetros de saída
    usable_start = reinterpret_cast<uint64_t>(base_addr);
    usable_size = total_size;  // TODO O ESPAÇO É PARA DADOS!
    return true;
}

// ============================================================
// 2. ALOCAR BLOCOS (FIRST-FIT)
// ============================================================

int64_t allocate_blocks(BlockAllocator& alloc, uint64_t thread_id, uint32_t num_blocks) {
    // 🔒 LOCK EXCLUSIVO PARA ACESSO AO BITMAP
    std::lock_guard<std::mutex> lock(alloc.mtx);
    
    // ============================================================
    // VALIDAÇÕES IMEDIATAS
    // ============================================================
    
    if (num_blocks == 0u) {
        return -1;
    }
    if (alloc.active_loans >= alloc.max_loans) {
        return -1;
    }
    if (alloc.bitmap_ptr == nullptr || alloc.loan_table_ptr == nullptr) {
        return -1;
    }

    // Verifica se a thread já possui empréstimo ativo
    for (uint32_t i = 0u; i < alloc.active_loans; ++i) {
        if (alloc.loan_table_ptr[i].id == thread_id) {
            return -2; // Thread já tem empréstimo
        }
    }

    // ============================================================
    // FIRST-FIT SEARCH
    // ============================================================
    
    const size_t total = static_cast<size_t>(alloc.total_blocks);
    size_t consecutive = 0u;
    size_t found_start = 0u;

    for (size_t i = 0u; i < total; ++i) {
        if (is_bit_set(alloc.bitmap_ptr, i)) {
            consecutive = 0u;
        } else {
            if (consecutive == 0u) {
                found_start = i;
            }
            ++consecutive;
            
            if (consecutive == static_cast<size_t>(num_blocks)) {
                // ============================================================
                // MARCA OS BITS COMO OCUPADOS
                // ============================================================
                for (size_t j = found_start; j < found_start + num_blocks; ++j) {
                    set_bit(alloc.bitmap_ptr, j);
                }
                
                // ============================================================
                // REGISTRA O EMPRÉSTIMO
                // ============================================================
                auto* entry = &alloc.loan_table_ptr[alloc.active_loans];
                entry->id = thread_id;
                entry->start_block = static_cast<uint32_t>(found_start);
                entry->num_blocks = num_blocks;
                ++alloc.active_loans;
                
                return static_cast<int64_t>(found_start);
            }
        }
    }
    
    return -1; // Não encontrou espaço contíguo
}

// ============================================================
// 3. LIBERAR BLOCOS POR ID
// ============================================================

bool free_blocks_by_id(BlockAllocator& alloc, uint64_t thread_id) {
    // 🔒 LOCK EXCLUSIVO PARA ACESSO AO BITMAP
    std::lock_guard<std::mutex> lock(alloc.mtx);
    
    // ============================================================
    // VALIDAÇÕES IMEDIATAS
    // ============================================================
    
    if (alloc.bitmap_ptr == nullptr || alloc.loan_table_ptr == nullptr) {
        return false;
    }
    if (alloc.active_loans == 0u) {
        return false;
    }

    // ============================================================
    // PROCURA O EMPRÉSTIMO DA THREAD
    // ============================================================
    
    for (uint32_t i = 0u; i < alloc.active_loans; ++i) {
        if (alloc.loan_table_ptr[i].id == thread_id) {
            const uint32_t start = alloc.loan_table_ptr[i].start_block;
            const uint32_t num = alloc.loan_table_ptr[i].num_blocks;
            
            // ============================================================
            // DEFESA CONTRA CORRUPÇÃO DE METADADOS
            // ============================================================
            // Usamos uint64_t para evitar overflow no cálculo start + num
            const uint64_t end_idx = static_cast<uint64_t>(start) + num;
            const uint32_t limit = static_cast<uint32_t>(
                std::min(end_idx, static_cast<uint64_t>(alloc.total_blocks))
            );
            
            // ============================================================
            // LIMPA OS BITS NO BITMAP (LIBERA OS BLOCOS)
            // ============================================================
            for (uint32_t j = start; j < limit; ++j) {
                clear_bit(alloc.bitmap_ptr, j);
            }
            
            // ============================================================
            // REMOVE O EMPRÉSTIMO (SWAP COM O ÚLTIMO)
            // ============================================================
            --alloc.active_loans;
            if (i < alloc.active_loans) {
                alloc.loan_table_ptr[i] = alloc.loan_table_ptr[alloc.active_loans];
            }
            
            return true;
        }
    }
    
    return false; // Thread não encontrada
}

// ============================================================
// 4. FUNÇÕES DE LEITURA SEGURAS (THREAD-SAFE)
// ============================================================

bool is_block_allocated(const BlockAllocator& alloc, uint32_t block_index) {
    // 🔒 LOCK PARA LEITURA CONSISTENTE
    // Nota: 'mtx' é mutable na struct BlockAllocator, permitindo lock em const
    std::lock_guard<std::mutex> lock(alloc.mtx);
    
    if (block_index >= alloc.total_blocks) {
        return false;
    }
    
    return is_bit_set(alloc.bitmap_ptr, static_cast<size_t>(block_index));
}

uint32_t get_active_loans(const BlockAllocator& alloc) {
    // 🔒 LOCK PARA LEITURA CONSISTENTE
    std::lock_guard<std::mutex> lock(alloc.mtx);
    return alloc.active_loans;
}

} // namespace memorymanager