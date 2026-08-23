#include "../include/cookie.hpp"
#include <cstring>
#include <cctype>

namespace http {

size_t parseCookies(const char* cookieHeader, char* keys[], size_t keySizes[],
                    char* values[], size_t valueSizes[], size_t maxPairs) {
    if (!cookieHeader || maxPairs == 0) return 0;

    size_t count = 0;
    const char* p = cookieHeader;
    while (*p && count < maxPairs) {
        // Pular espaços iniciais
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0') break;

        // Encontrar '='
        const char* eq = strchr(p, '=');
        if (!eq) break;

        // Extrair chave
        size_t keyLen = eq - p;
        if (keyLen > 0 && keyLen < keySizes[count]) {
            memcpy(keys[count], p, keyLen);
            keys[count][keyLen] = '\0';
        } else {
            // Chave muito longa ou vazia, ignorar este cookie
            p = eq + 1;
            // Avançar até ';' ou fim
            while (*p && *p != ';') ++p;
            if (*p == ';') ++p;
            continue;
        }

        // Valor começa após '='
        const char* valStart = eq + 1;
        const char* valEnd = valStart;
        while (*valEnd && *valEnd != ';') ++valEnd;

        size_t valLen = valEnd - valStart;
        if (valLen > 0 && valLen < valueSizes[count]) {
            memcpy(values[count], valStart, valLen);
            values[count][valLen] = '\0';
        } else {
            // Valor muito longo, ignorar
            p = valEnd;
            if (*p == ';') ++p;
            continue;
        }

        ++count;
        p = valEnd;
        if (*p == ';') ++p;
    }
    return count;
}

} // namespace http