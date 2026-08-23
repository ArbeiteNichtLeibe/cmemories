#ifndef TPM2_MANAGER_HPP
#define TPM2_MANAGER_HPP

#include "tpm2_info.hpp"

namespace memorymanager {
    class MemoryManagerThread;
}

namespace tpm2 {

class TPMManager {
public:
    TPMManager();
    ~TPMManager();
    
    bool getPepper(uint8_t* out_buffer, size_t buffer_size,
                   char* out_error = nullptr, size_t err_size = 0) const;
    bool init(memorymanager::MemoryManagerThread* memory_manager,
              const char* pub_path, const char* priv_path,
              char* out_error, size_t err_size);

    void shutdown();

private:
    Session session_{};
    Key key_{};
    bool ready_{false};
    uint8_t pepper_[64] = {}; 
};

} // namespace tpm2

#endif // TPM2_MANAGER_HPP