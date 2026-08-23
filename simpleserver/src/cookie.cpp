#include "cookie.hpp"
#include <cstring>
#include <cctype>

namespace http {
namespace cookie {

size_t parse_cookies(const char* cookie_header,
                     CookiePair* pairs, size_t max_pairs) {
    if (!cookie_header || !pairs || max_pairs == 0) return 0;

    size_t count = 0;
    const char* p = cookie_header;
    while (*p && count < max_pairs) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\0') break;

        const char* eq = strchr(p, '=');
        if (!eq) break;

        size_t key_len = eq - p;
        if (key_len == 0 || key_len >= sizeof(CookiePair::key)) {
            p = eq + 1;
            while (*p && *p != ';') ++p;
            if (*p == ';') ++p;
            continue;
        }
        std::memcpy(pairs[count].key, p, key_len);
        pairs[count].key[key_len] = '\0';

        const char* val_start = eq + 1;
        const char* val_end = val_start;
        while (*val_end && *val_end != ';') ++val_end;
        size_t val_len = val_end - val_start;
        if (val_len == 0 || val_len >= sizeof(CookiePair::value)) {
            p = val_end;
            if (*p == ';') ++p;
            continue;
        }
        std::memcpy(pairs[count].value, val_start, val_len);
        pairs[count].value[val_len] = '\0';

        ++count;
        p = val_end;
        if (*p == ';') ++p;
    }
    return count;
}

} // namespace cookie
} // namespace http