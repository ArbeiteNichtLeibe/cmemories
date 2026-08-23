#include "key_generator.hpp"
#include "../../uteis/include/uteis.hpp"
#include <openssl/sha.h>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <grp.h>

namespace KeyGenerator {

class FileGuard {
public:
    explicit FileGuard(int fd = -1) : fd_(fd) {}
    ~FileGuard() { if (fd_ >= 0) close(fd_); }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    int get() const { return fd_; }
    bool isValid() const { return fd_ >= 0; }
private:
    int fd_;
};

static bool getRandomBytes(uint8_t* buffer, size_t len, char* outError, size_t errSize) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        Utils::safeCopyString(outError, errSize, "Cannot open /dev/urandom");
        return false;
    }
    FileGuard guard(fd);
    ssize_t n = read(fd, buffer, len);
    if (n != static_cast<ssize_t>(len)) {
        Utils::safeCopyString(outError, errSize, "Failed to read enough random bytes");
        return false;
    }
    return true;
}

static void hashToHex(const uint8_t hash[SHA256_DIGEST_LENGTH], char hex[65]) {
    static const char hexChars[] = "0123456789abcdef";
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        hex[i * 2]     = hexChars[(hash[i] >> 4) & 0x0F];
        hex[i * 2 + 1] = hexChars[hash[i] & 0x0F];
    }
    hex[64] = '\0';
}

// Salva a chave em /run/memorandos/ com permissão 0644 e grupo www-data
static bool saveKeyToFile(const char* keyHex, const char* filename, char* outError, size_t errSize) {
    // Remove antigo se existir
    unlink(filename);

    // Cria com permissões 0644 (leitura para grupo e outros, escrita apenas dono)
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(outError, errSize, "Cannot open %s: %s", filename, strerror(errno));
        return false;
    }
    FileGuard guard(fd);

    // Tenta mudar grupo para www-data (não crítico)
    struct group* grp = getgrnam("www-data");
    if (grp != nullptr) {
        fchown(fd, -1, grp->gr_gid);
    }

    // Força permissões 0644
    fchmod(fd, 0644);

    size_t len = std::strlen(keyHex);
    ssize_t written = write(fd, keyHex, len);
    if (written != static_cast<ssize_t>(len)) {
        snprintf(outError, errSize, "Write error to %s", filename);
        return false;
    }
    return true;
}

bool generateKeys(memorymanager::MemoryManagerThread* mm,
                  tpm2::TPMManager* tpm_man,
                  char* outError,
                  size_t errSize) {
    if (!mm || !tpm_man) {
        Utils::safeCopyString(outError, errSize, "Null pointer(s) provided");
        return false;
    }

    // Obter pepper do TPM (64 bytes)
    uint8_t pepper[64];
    char errBuf[256] = {0};
    if (!tpm_man->getPepper(pepper, sizeof(pepper), errBuf, sizeof(errBuf))) {
        Utils::safeCopyString(outError, errSize, errBuf);
        return false;
    }

    // Usamos buffer na stack (não precisa de alocação via MemoryManager)
    char combined[256];

    // --- Chave JWT: pepper + random salt ---
    uint8_t randomSalt[32];
    if (!getRandomBytes(randomSalt, sizeof(randomSalt), outError, errSize)) {
        return false;
    }

    char* ptr = combined;
    std::memcpy(ptr, pepper, sizeof(pepper));
    ptr += sizeof(pepper);
    std::memcpy(ptr, randomSalt, sizeof(randomSalt));
    ptr += sizeof(randomSalt);
    size_t totalJwtLen = sizeof(pepper) + sizeof(randomSalt);

    uint8_t hashJwt[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(combined), totalJwtLen, hashJwt);
    char jwtHex[65];
    hashToHex(hashJwt, jwtHex);

    // --- Chave para senhas: pepper + salt fixo ---
    static const char* FIXED_SALT = "O UMBRAL me faz querer estar contigo mais uma vez!!";
    size_t fixedSaltLen = std::strlen(FIXED_SALT);

    ptr = combined;
    std::memcpy(ptr, pepper, sizeof(pepper));
    ptr += sizeof(pepper);
    std::memcpy(ptr, FIXED_SALT, fixedSaltLen);
    size_t totalPwdLen = sizeof(pepper) + fixedSaltLen;

    uint8_t hashPwd[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(combined), totalPwdLen, hashPwd);
    char pwdHex[65];
    hashToHex(hashPwd, pwdHex);

    // Salvar arquivos no diretório /run/memorandos/
    if (!saveKeyToFile(jwtHex, "/run/memorandos/jwt_chave.conf", outError, errSize)) return false;
    if (!saveKeyToFile(pwdHex, "/run/memorandos/discreto.conf", outError, errSize)) return false;

    return true;
}

} // namespace KeyGenerator