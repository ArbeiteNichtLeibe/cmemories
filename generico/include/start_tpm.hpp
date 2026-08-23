#ifndef START_TPM_HPP
#define START_TPM_HPP

#include "../../memorymanager/include/memory_manager_thread.hpp"
#include "../../tpm2/include/tpm2_manager.hpp"
#include <cstddef>
#include <cstdint>

namespace TPMStart {

bool initTPM(memorymanager::MemoryManagerThread* memory_manager);

tpm2::TPMManager& getTPMManager();

void shutdownTPM();

bool deriveKeyFromPepper(memorymanager::MemoryManagerThread* mm,
                         const uint8_t* salt, size_t salt_len,
                         uint8_t* out_key, size_t out_len,
                         char* out_error, size_t err_size);

} // namespace TPMStart

#endif // START_TPM_HPP