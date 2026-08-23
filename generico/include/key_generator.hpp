#ifndef KEY_GENERATOR_HPP
#define KEY_GENERATOR_HPP

#include "../../memorymanager/include/memory_manager_thread.hpp"
#include "../../tpm2/include/tpm2_manager.hpp"
#include <cstddef>

namespace KeyGenerator {

bool generateKeys(memorymanager::MemoryManagerThread* mm,
                  tpm2::TPMManager* tpm_man,
                  char* outError = nullptr,
                  size_t errSize = 0);

} // namespace KeyGenerator

#endif