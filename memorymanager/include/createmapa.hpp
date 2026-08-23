// createmmapa.hpp
#ifndef CREATEMmapA_HPP
#define CREATEMmapA_HPP

#include <cstddef>
#include <cstdint>

namespace memorymanager {

// ============================================================
// CONSTANTES PÚBLICAS (Limites do Sistema)
// ============================================================

/**
 * @brief Tamanho mínimo da região mmap (1GB).
 * 
 * O sistema precisa de pelo menos 1GB para ser útil,
 * pois os blocos são de 1MB e precisamos de um número
 * razoável de blocos para operações.
 */
static constexpr size_t MIN_REGION_SIZE = 1024ULL * 1024ULL * 1024ULL; // 1GB

/**
 * @brief Tamanho máximo da região mmap (30GB).
 * 
 * O sistema tem 48GB de RAM total. 30GB é o máximo
 * seguro para deixar espaço para o sistema operacional
 * e outros processos.
 */
static constexpr size_t MAX_REGION_SIZE = 30ULL * 1024ULL * 1024ULL * 1024ULL; // 30GB

// ============================================================
// FUNÇÕES PÚBLICAS
// ============================================================

/**
 * @brief Allocates a memory region using mmap.
 * 
 * @param requested_size Size of the region (must be between 1GB and 30GB)
 * @param base           [out] Pointer to the allocated region
 * @param total_size     [out] Actual size allocated
 * @return true if allocation succeeded
 */
bool allocate_region(size_t requested_size, void*& base, size_t& total_size);

/**
 * @brief Frees a memory region previously allocated.
 * 
 * @param base        Pointer to the region
 * @param total_size  Size of the region
 * @return true if freeing succeeded
 */
bool free_region(void* base, size_t total_size);

/**
 * @brief Checks if a memory region is currently allocated.
 * 
 * @return true if a region is allocated
 */
bool is_region_allocated();

/**
 * @brief Gets information about the currently allocated region.
 * 
 * @param base  [out] Pointer to the region
 * @param size  [out] Size of the region
 * @return true if a region is allocated
 */
bool get_region_info(void*& base, size_t& size);

/**
 * @brief Gets the minimum allowed region size (1GB).
 * 
 * @return size_t Minimum region size in bytes
 */
constexpr size_t get_min_region_size() {
    return MIN_REGION_SIZE;
}

/**
 * @brief Gets the maximum allowed region size (30GB).
 * 
 * @return size_t Maximum region size in bytes
 */
constexpr size_t get_max_region_size() {
    return MAX_REGION_SIZE;
}

} // namespace memorymanager

#endif // CREATEMmapA_HPP