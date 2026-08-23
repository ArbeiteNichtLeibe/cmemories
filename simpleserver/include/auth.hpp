#pragma once

#include <cstddef>
#include <cstdint>
#include "../../tpm2/include/tpm2_manager.hpp"   // include completo

namespace http {
namespace auth {

// Carrega ou gera o token a partir do pepper do TPM e persiste no arquivo.
bool load_or_generate_token(const char* config_path,
                            tpm2::TPMManager* tpm_manager,
                            char* out_error, size_t err_size);

// Verifica se o token recebido (64 caracteres hex) é igual ao armazenado.
bool verify_token(const char* token_hex, size_t token_len);

// Retorna o token armazenado (apenas para uso interno, não modificar).
const char* get_stored_token();

} // namespace auth
} // namespace http