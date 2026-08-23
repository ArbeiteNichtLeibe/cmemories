#include "../include/tpm2_manager.hpp"
#include "../../uteis/include/uteis.hpp"
#include <cstring>
#include <iostream>

namespace tpm2 {

TPMManager::TPMManager() = default;

TPMManager::~TPMManager() {
    shutdown();
}

void TPMManager::shutdown() {
    if (ready_) {
        // Se houver chaves de sessão carregadas, descarrega
        unload_key(session_, key_);
        ready_ = false;
    }
}

bool TPMManager::init(memorymanager::MemoryManagerThread* /* memory_manager */,
                      const char* /* pub_path */, const char* /* priv_path */,
                      char* out_error, size_t err_size) {
    if (ready_) return true;

    // Verifica se a sessão do TSS2/Esys está válida
    if (!session_.is_valid()) {
        if (out_error && err_size > 0) {
            Utils::safeCopyString(out_error, err_size, "Failed to initialize TPM 2.0 session");
        }
        return false;
    }

    // Tenta ler o pepper existente ou criar um novo (gera e grava no TPM apenas na primeira vez)
    uint8_t pepper[64] = {0};
    if (!obtain_or_create_pepper(session_, pepper, out_error, err_size)) {
        return false;
    }

    // Armazena o pepper na memória do objeto para acesso rápido via getPepper()
    std::memcpy(pepper_, pepper, sizeof(pepper_));

    ready_ = true;
    return true;
}

bool TPMManager::getPepper(uint8_t* out_buffer, size_t buffer_size,
                           char* out_error, size_t err_size) const {
    if (!ready_) {
        if (out_error && err_size > 0) {
            Utils::safeCopyString(out_error, err_size, "TPM not initialized");
        }
        return false;
    }
    
    // Validação de segurança para o buffer de destino
    if (!out_buffer || buffer_size < sizeof(pepper_)) {
        if (out_error && err_size > 0) {
            Utils::safeCopyString(out_error, err_size, "Buffer too small or null");
        }
        return false;
    }

    std::memcpy(out_buffer, pepper_, sizeof(pepper_));
    return true;
}

} // namespace tpm2