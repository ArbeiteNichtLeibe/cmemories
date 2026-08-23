#pragma once

#include <cstddef>

namespace http {
namespace cookie {

// Estrutura para armazenar um par chave/valor de cookie.
struct CookiePair {
    char key[64];
    char value[256];
};

// Extrai cookies do header e preenche um array de CookiePair.
// Retorna o número de cookies extraídos (máximo max_pairs).
// Os buffers são fornecidos pelo chamador (normalmente alocados na arena).
size_t parse_cookies(const char* cookie_header,
                     CookiePair* pairs, size_t max_pairs);

} // namespace cookie
} // namespace http