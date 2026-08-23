// block_address_calculator.hpp
#ifndef BLOCK_ADDRESS_CALCULATOR_HPP
#define BLOCK_ADDRESS_CALCULATOR_HPP

#include <cstdint>
#include <cstddef>

namespace memorymanager {

/**
 * @brief Estrutura mínima necessária para cálculos de endereço.
 * 
 * Esta é uma versão reduzida do BlockAllocator contendo apenas
 * os campos necessários para cálculos de endereço.
 * Não depende de BlockAllocator, LoanEntry ou qualquer outra estrutura.
 */
struct BlockAllocatorAddressInfo {
    void* data_start{nullptr};     ///< Ponteiro para o início dos dados úteis
    size_t block_size{0u};         ///< Tamanho de cada bloco em bytes
    uint32_t total_blocks{0u};     ///< Número total de blocos na arena
};

/**
 * @brief Obtém o endereço de um único bloco dentro da arena alocada.
 * 
 * @param alloc         Informações mínimas do alocador
 * @param block_index   Índice do bloco (0-based)
 * @param start_address [saída] Ponteiro para o início do bloco (void*)
 * @param end_address   [saída] Ponteiro para o fim do bloco (exclusive)
 * @param out_error     [saída] Buffer para mensagem de erro (pode ser nullptr)
 * @param err_size      Tamanho do buffer out_error (0 se não usado)
 * @return true se o bloco é válido, false em caso de erro
 */
bool get_block_address(const BlockAllocatorAddressInfo& alloc,
                       uint32_t block_index,
                       void*& start_address,
                       void*& end_address,
                       char* out_error = nullptr,
                       size_t err_size = 0u);

/**
 * @brief Obtém o endereço de um intervalo contíguo de blocos.
 * 
 * @param alloc         Informações mínimas do alocador
 * @param start_block   Índice do primeiro bloco (0-based)
 * @param num_blocks    Número de blocos contíguos
 * @param start_address [saída] Ponteiro para o início do intervalo (void*)
 * @param end_address   [saída] Ponteiro para o fim do intervalo (exclusive)
 * @param out_error     [saída] Buffer para mensagem de erro (pode ser nullptr)
 * @param err_size      Tamanho do buffer out_error (0 se não usado)
 * @return true se o intervalo é válido, false em caso de erro
 */
bool get_block_range_address(const BlockAllocatorAddressInfo& alloc,
                             uint32_t start_block,
                             uint32_t num_blocks,
                             void*& start_address,
                             void*& end_address,
                             char* out_error = nullptr,
                             size_t err_size = 0u);

// ============================================================
// FUNÇÕES DE CONVENIÊNCIA (SEM BUFFER DE ERRO)
// ============================================================

inline bool get_block_address_simple(const BlockAllocatorAddressInfo& alloc,
                                     uint32_t block_index,
                                     void*& start_address,
                                     void*& end_address) {
    return get_block_address(alloc, block_index, start_address, end_address, nullptr, 0u);
}

inline bool get_block_range_address_simple(const BlockAllocatorAddressInfo& alloc,
                                           uint32_t start_block,
                                           uint32_t num_blocks,
                                           void*& start_address,
                                           void*& end_address) {
    return get_block_range_address(alloc, start_block, num_blocks, 
                                   start_address, end_address, nullptr, 0u);
}

// ============================================================
// FUNÇÕES PARA ACESSO TIPADO (COM uint8_t*)
// ============================================================

inline bool get_block_address_u8(const BlockAllocatorAddressInfo& alloc,
                                 uint32_t block_index,
                                 uint8_t*& start_address,
                                 uint8_t*& end_address,
                                 char* out_error = nullptr,
                                 size_t err_size = 0u) {
    void* start_void = nullptr;
    void* end_void = nullptr;
    
    if (!get_block_address(alloc, block_index, start_void, end_void, out_error, err_size)) {
        start_address = nullptr;
        end_address = nullptr;
        return false;
    }
    
    start_address = reinterpret_cast<uint8_t*>(start_void);
    end_address = reinterpret_cast<uint8_t*>(end_void);
    return true;
}

inline bool get_block_range_address_u8(const BlockAllocatorAddressInfo& alloc,
                                       uint32_t start_block,
                                       uint32_t num_blocks,
                                       uint8_t*& start_address,
                                       uint8_t*& end_address,
                                       char* out_error = nullptr,
                                       size_t err_size = 0u) {
    void* start_void = nullptr;
    void* end_void = nullptr;
    
    if (!get_block_range_address(alloc, start_block, num_blocks, 
                                 start_void, end_void, out_error, err_size)) {
        start_address = nullptr;
        end_address = nullptr;
        return false;
    }
    
    start_address = reinterpret_cast<uint8_t*>(start_void);
    end_address = reinterpret_cast<uint8_t*>(end_void);
    return true;
}

// ============================================================
// FUNÇÕES DE UTILIDADE
// ============================================================

inline size_t get_block_offset(const BlockAllocatorAddressInfo& alloc, uint32_t block_index) {
    return static_cast<size_t>(block_index) * alloc.block_size;
}

inline bool is_valid_block_index(const BlockAllocatorAddressInfo& alloc, uint32_t block_index) {
    return block_index < alloc.total_blocks;
}

inline bool is_valid_block_range(const BlockAllocatorAddressInfo& alloc,
                                 uint32_t start_block,
                                 uint32_t num_blocks) {
    if (num_blocks == 0u) return false;
    if (start_block >= alloc.total_blocks) return false;
    const uint64_t end_idx = static_cast<uint64_t>(start_block) + num_blocks;
    return end_idx <= alloc.total_blocks;
}

} // namespace memorymanager

#endif // BLOCK_ADDRESS_CALCULATOR_HPP