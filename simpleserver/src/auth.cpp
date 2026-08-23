#include "auth.hpp"
#include "../../tpm2/include/tpm2_manager.hpp"
#include "../../uteis/include/uteis.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <cstring>
#include <cctype>
#include <cstdio>

namespace http {
namespace auth {

namespace {
    static char g_auth_token[65] = {0};
    static bool g_token_loaded = false;

    static void compute_token_hex(const uint8_t* pepper, size_t pepper_len,
                                  const char* salt, size_t salt_len,
                                  char* out_hex) {
        const size_t total_len = pepper_len + salt_len;
        unsigned char combined[128];
        std::memcpy(combined, pepper, pepper_len);
        std::memcpy(combined + pepper_len, salt, salt_len);

        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len = 0;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        const EVP_MD* md = EVP_sha256();
        EVP_DigestInit_ex(ctx, md, nullptr);
        EVP_DigestUpdate(ctx, combined, total_len);
        EVP_DigestFinal_ex(ctx, hash, &hash_len);
        EVP_MD_CTX_free(ctx);

        for (size_t i = 0; i < hash_len && i < 32; ++i) {
            sprintf(out_hex + (i * 2), "%02x", hash[i]);
        }
        out_hex[64] = '\0';
    }

    static bool read_token_from_file(const char* path, char* out_token,
                                     char* out_error, size_t err_size) {
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;

        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            Utils::safeCopyString(out_error, err_size, "Cannot open token file");
            return false;
        }
        char buffer[4096];
        ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (n <= 0) {
            Utils::safeCopyString(out_error, err_size, "Token file empty or read error");
            return false;
        }
        buffer[n] = '\0';

        const char* prefix = "auth_token=";
        const size_t prefix_len = 11;
        char* pos = strstr(buffer, prefix);
        if (!pos) {
            Utils::safeCopyString(out_error, err_size, "Token not found in file");
            return false;
        }
        pos += prefix_len;
        while (*pos == ' ' || *pos == '\t') ++pos;

        size_t i = 0;
        while (i < 64 && pos[i] && pos[i] != '\n' && pos[i] != '\r') {
            if (!isxdigit(pos[i])) {
                Utils::safeCopyString(out_error, err_size, "Invalid hex token");
                return false;
            }
            out_token[i] = pos[i];
            ++i;
        }
        if (i != 64) {
            Utils::safeCopyString(out_error, err_size, "Token must be 64 hex chars");
            return false;
        }
        out_token[64] = '\0';
        return true;
    }

    static bool write_token_to_file(const char* path, const char* token,
                                    char* out_error, size_t err_size) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            Utils::safeCopyString(out_error, err_size, "Cannot create token file");
            return false;
        }
        char line[128];
        int len = snprintf(line, sizeof(line), "auth_token=%s\n", token);
        ssize_t written = write(fd, line, len);
        close(fd);
        if (written != len) {
            Utils::safeCopyString(out_error, err_size, "Failed to write token");
            return false;
        }
        return true;
    }
} // anonymous namespace

bool load_or_generate_token(const char* config_path,
                            tpm2::TPMManager* tpm_manager,
                            char* out_error, size_t err_size) {
    if (g_token_loaded) return true;
    if (!tpm_manager) {
        Utils::safeCopyString(out_error, err_size, "TPM manager is null");
        return false;
    }

    char token[65] = {0};
    if (read_token_from_file(config_path, token, out_error, err_size)) {
        std::memcpy(g_auth_token, token, 65);
        g_token_loaded = true;
        return true;
    }

    uint8_t pepper[64] = {0};
    char tpm_err[256] = {0};
    if (!tpm_manager->getPepper(pepper, sizeof(pepper), tpm_err, sizeof(tpm_err))) {
        Utils::safeCopyString(out_error, err_size, tpm_err);
        return false;
    }

    const char* salt = "Onegai, bite, please";
    compute_token_hex(pepper, 64, salt, strlen(salt), token);

    if (!write_token_to_file(config_path, token, out_error, err_size)) {
        return false;
    }

    std::memcpy(g_auth_token, token, 65);
    g_token_loaded = true;
    return true;
}

bool verify_token(const char* token_hex, size_t token_len) {
    if (!g_token_loaded || !token_hex || token_len != 64) return false;
    return (std::memcmp(token_hex, g_auth_token, 64) == 0);
}

const char* get_stored_token() {
    return g_token_loaded ? g_auth_token : nullptr;
}

} // namespace auth
} // namespace http