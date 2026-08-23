#ifndef TPM2_INFO_HPP
#define TPM2_INFO_HPP

#include <cstddef>
#include <cstdint>
#include <tss2/tss2_esys.h> // Inclui os tipos reais do TPM (ESYS_CONTEXT)

namespace tpm2 {

class Session {
public:
    Session();
    ~Session();

    bool is_valid() const { return valid_; }
    void* get_esys() { return static_cast<void*>(esys_ctx_); }

private:
    ESYS_CONTEXT* esys_ctx_{nullptr};
    bool valid_{false};
};

struct Info {
    bool success{false};
    char manufacturer[64]{0};
    char vendor_string[64]{0};
};

struct Key {
    char name[64]{0};
    bool loaded{false};
};

Info get_info(Session& session, char* out_error, size_t err_size);
bool obtain_or_create_pepper(Session& session, uint8_t* out_pepper_64b, char* out_error, size_t err_size);

bool generate_key(Session& session, Key& key, const char* name, char* out_error, size_t err_size);
bool load_keys(Session& session, Key& key, const char* pub, const char* priv, char* out_error, size_t err_size);
bool save_keys(const Key& key, const char* pub, const char* priv, char* out_error, size_t err_size);
void unload_key(Session& session, Key& key);

} // namespace tpm2

#endif // TPM2_INFO_HPP