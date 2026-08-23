#include "../include/auth.hpp"
#include "../../tpm2/include/tpm2_manager.hpp"
#include "../../uteis/include/uteis.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/sha.h>

namespace http {

namespace {
    // Buffer estático para armazenar o token (64 caracteres hex + '\0')
    static char g_auth_token[65] = {0};
    static bool g_token_loaded = false;

    // Calcula SHA-256 de (pepper + salt) e converte para hex
    static void compute_token_hex(const uint8_t* pepper, size_t pepper_len,
                                  const char* salt, size_t salt_len,
                                  char* out_hex) {
        uint8_t hash[SHA256_DIGEST_LENGTH];
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, pepper, pepper_len);
        SHA256_Update(&ctx, salt, salt_len);
        SHA256_Final(hash, &ctx);

        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            sprintf(out_hex + (i * 2), "%02x", hash[i]);
        }
        out_hex[64] = '\0';
    }

    // Lê o arquivo e extrai a linha "auth_token=..."
    static bool read_token_from_file(const char* path, char* out_token,
                                     size_t token_size, char* out_error,
                                     size_t err_size) {
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            return false; // arquivo não existe ou não é regular
        }

        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            Utils::safeCopyString(out_error, err_size, "Falha ao abrir arquivo de token");
            return false;
        }

        // Lê todo o conteúdo (até 4096 bytes) – usamos um buffer local na stack,
        // mas é pequeno e temporário.
        char buffer[4096];
        ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (n <= 0) {
            Utils::safeCopyString(out_error, err_size, "Arquivo de token vazio ou erro de leitura");
            return false;
        }
        buffer[n] = '\0';

        // Procura pela linha "auth_token="
        const char* prefix = "auth_token=";
        const size_t prefix_len = 11;
        char* pos = strstr(buffer, prefix);
        if (!pos) {
            Utils::safeCopyString(out_error, err_size, "Token não encontrado no arquivo");
            return false;
        }
        pos += prefix_len;
        // Pular espaços em branco
        while (*pos == ' ' || *pos == '\t') ++pos;

        // Copiar até o fim da linha ou até 64 caracteres hex
        size_t i = 0;
        while (i < 64 && pos[i] && pos[i] != '\n' && pos[i] != '\r') {
            if (!isxdigit(pos[i])) {
                Utils::safeCopyString(out_error, err_size, "Token inválido (caractere não hexadecimal)");
                return false;
            }
            out_token[i] = pos[i];
            ++i;
        }
        if (i != 64) {
            Utils::safeCopyString(out_error, err_size, "Token deve ter exatamente 64 caracteres hex");
            return false;
        }
        out_token[64] = '\0';
        return true;
    }

    // Grava o token no arquivo (substitui conteúdo)
    static bool write_token_to_file(const char* path, const char* token,
                                    char* out_error, size_t err_size) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            Utils::safeCopyString(out_error, err_size, "Falha ao criar arquivo de token");
            return false;
        }

        char line[128];
        int len = snprintf(line, sizeof(line), "auth_token=%s\n", token);
        ssize_t written = write(fd, line, len);
        close(fd);
        if (written != len) {
            Utils::safeCopyString(out_error, err_size, "Falha ao escrever token no arquivo");
            return false;
        }
        return true;
    }
} // anonymous namespace

bool loadOrGenerateAuthToken(const char* configPath, char* outError, size_t errSize) {
    if (g_token_loaded) return true; // já carregado

    // Tenta ler do arquivo
    char token[65] = {0};
    if (read_token_from_file(configPath, token, sizeof(token), outError, errSize)) {
        // Token válido, armazena e retorna
        memcpy(g_auth_token, token, 65);
        g_token_loaded = true;
        return true;
    }

    // Se não leu, gera novo a partir do pepper do TPM
    uint8_t pepper[64] = {0};
    char tpmErr[256] = {0};
    if (!TPMStart::getTPMManager().getPepper(pepper, sizeof(pepper),
                                             tpmErr, sizeof(tpmErr))) {
        Utils::safeCopyString(outError, errSize, tpmErr);
        return false;
    }

    const char* salt = "webserver_salt";
    compute_token_hex(pepper, 64, salt, strlen(salt), token);

    // Persiste no arquivo
    if (!write_token_to_file(configPath, token, outError, errSize)) {
        return false;
    }

    memcpy(g_auth_token, token, 65);
    g_token_loaded = true;
    return true;
}

bool verifyBearerToken(const char* token, size_t tokenLen) {
    if (!g_token_loaded || !token || tokenLen != 64) return false;
    // Comparação de 64 caracteres sem usar std::string
    return (tokenLen == 64 && memcmp(token, g_auth_token, 64) == 0);
}

const char* getStoredAuthToken() {
    return g_token_loaded ? g_auth_token : nullptr;
}

} // namespace http