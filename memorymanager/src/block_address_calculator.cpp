// block_address_calculator.cpp
#include "../include/block_address_calculator.hpp"
#include "../../uteis/include/uteis.hpp"  // ADICIONADO: para safeCopyString
#include <cstddef>
#include <limits>

namespace memorymanager {

/**
 * @brief Verifica se a multiplicação a * b causaria overflow em size_t.
 * 
 * @param a       Primeiro operando
 * @param b       Segundo operando
 * @param result  [saída] Resultado da multiplicação (se não houver overflow)
 * @return true se houve overflow, false caso contrário
 */
static bool check_multiplication_overflow(size_t a, size_t b, size_t& result) {
    // Verifica se b > max / a (prevenção de divisão por zero)
    if (a != 0u && b > (std::numeric_limits<size_t>::max() / a)) {
        return true; // Overflow detectado
    }
    result = a * b;
    return false;
}

bool get_block_address(const BlockAllocatorAddressInfo& alloc,
                       uint32_t block_index,
                       void*& start_address,
                       void*& end_address,
                       char* out_error,
                       size_t err_size) {
    // Validação: data_start não pode ser nulo
    if (alloc.data_start == nullptr) {
        start_address = nullptr;
        end_address = nullptr;
        Utils::safeCopyString(out_error, err_size, "Data start pointer is null.");  // MODIFICADO
        return false;
    }

    // Validação: block_size não pode ser zero
    if (alloc.block_size == 0u) {
        start_address = nullptr;
        end_address = nullptr;
        Utils::safeCopyString(out_error, err_size, "Block size cannot be zero.");  // MODIFICADO
        return false;
    }

    // Validação: block_index dentro dos limites
    if (block_index >= alloc.total_blocks) {
        start_address = nullptr;
        end_address = nullptr;
        Utils::safeCopyString(out_error, err_size, "Block index out of bounds.");  // MODIFICADO
        return false;
    }

    // Cálculo do offset com verificação de overflow
    size_t start_offset = 0u;
    if (check_multiplication_overflow(static_cast<size_t>(block_index), 
                                       alloc.block_size, 
                                       start_offset)) {
        start_address = nullptr;
        end_address = nullptr;
        Utils::safeCopyString(out_error, err_size, "Address calculation integer overflow.");  // MODIFICADO
        return false;
    }

    // Cálculo dos endereços
    // Nota: Usamos reinterpret_cast porque convertemos void* para uint8_t*
    // para aritmética de ponteiros. O padrão C++ permite reinterpret_cast
    // para conversão entre ponteiros de tipos relacionados (void* e uint8_t*).
    auto* data = reinterpret_cast<uint8_t*>(alloc.data_start);
    start_address = reinterpret_cast<void*>(data + start_offset);
    end_address = reinterpret_cast<void*>(data + start_offset + alloc.block_size);
    return true;
}

bool get_block_range_address(const BlockAllocatorAddressInfo& alloc,
                             uint32_t start_block,
                             uint32_t num_blocks,
                             void*& start_address,
                             void*& end_address,
                             char* out_error,
                             size_t err_size) {
    // Validação: data_start não pode ser nulo
    if (alloc.data_start == nullptr) {
        start_address = nullptr;
        end_address = nullptr;
        Utils::safeCopyString(out_error, err_size, "Data start pointer is null.");  // MODIFICADO
        return false;
    }

    // Validação: block_size não pode ser zero
    if (alloc.block_size == 0u) {
        start_address = nullptr;
        end_address = nullptr;
        Utils::safeCopyString(out_error, err_size, "Block size cannot be zero.");  // MODIFICADO
        return false;
    }

    // Validação: start_block dentro dos limites
    if (start_block >= alloc.total_blocks) {
        start_address = nullptr;
        end_address = nullptr;
        Utils::safeCopyString(out_error, err_size, "Start block out of bounds.");  // MODIFICADO
        return false;
    }

    // Validação: num_blocks não pode ser zero
    if (num_blocks == 0u) {
        start_address = nullptr;
        end_address = nullptr;
        Utils::safeCopyString(out_error, err_size, "Number of blocks requested is zero.");  // MODIFICADO
        return false;
    }

    // Cálculo do índice final com validação de overflow
    const uint64_t end_idx = static_cast<uint64_t>(start_block) + num_blocks;
    if (end_idx > alloc.total_blocks) {
        start_address = nullptr;
        end_address = nullptr;
        Utils::safeCopyString(out_error, err_size, "Block range exceeds total allocation size.");  // MODIFICADO
        return false;
    }

    // Cálculo dos offsets com verificação de overflow
    size_t start_offset = 0u;
    size_t end_offset = 0u;
    
    if (check_multiplication_overflow(static_cast<size_t>(start_block), 
                                       alloc.block_size, 
                                       start_offset) ||
        check_multiplication_overflow(static_cast<size_t>(end_idx), 
                                       alloc.block_size, 
                                       end_offset)) {
        start_address = nullptr;
        end_address = nullptr;
        Utils::safeCopyString(out_error, err_size, "Address calculation integer overflow.");  // MODIFICADO
        return false;
    }

    // Cálculo dos endereços
    auto* data = reinterpret_cast<uint8_t*>(alloc.data_start);
    start_address = reinterpret_cast<void*>(data + start_offset);
    end_address = reinterpret_cast<void*>(data + end_offset);
    return true;
}

} // namespace memorymanager