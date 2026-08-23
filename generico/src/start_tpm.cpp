#include "../include/start_tpm.hpp"
#include "../../uteis/include/uteis.hpp"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>

namespace TPMStart {

static tpm2::TPMManager g_tpm_manager;
static bool g_tpm_initialized = false;

bool initTPM(memorymanager::MemoryManagerThread* memory_manager) {
    if (g_tpm_initialized) return true;

   // std::cout << "\n🔒 Initializing TPM 2.0 Manager..." << std::endl;
    char tpm_err[256] = {0};

    // Chama com a assinatura atual (pub_path e priv_path são nullptr)
    if (!g_tpm_manager.init(memory_manager, nullptr, nullptr, tpm_err, sizeof(tpm_err))) {
        std::cerr << "❌ Erro ao inicializar TPM Manager: " << tpm_err << std::endl;
        return false;
    }

    // Obter pepper para teste
    uint8_t pepper[64] = {0};
    char pepper_err[256] = {0};
    if (!g_tpm_manager.getPepper(pepper, sizeof(pepper), pepper_err, sizeof(pepper_err))) {
        std::cerr << "⚠️ Falha ao obter pepper: " << pepper_err << std::endl;
        return false;
    }

    std::cout << "🔑 Pepper obtido com sucesso. Primeiros 3 bytes: "
              << std::hex << std::setw(2) << std::setfill('0')
              << (int)pepper[0] << " "
              << (int)pepper[1] << " "
              << (int)pepper[2] << std::dec << std::endl;

    // Marcar como inicializado ANTES do teste
    g_tpm_initialized = true;

    // ---------------------------------------------------------------------
    // TESTE DA FUNÇÃO deriveKeyFromPepper
    // ---------------------------------------------------------------------
    uint8_t test_key[32] = {0};
    char derive_err[256] = {0};
    const char* test_salt = "test_salt_123";

    if (deriveKeyFromPepper(memory_manager,
                            reinterpret_cast<const uint8_t*>(test_salt), strlen(test_salt),
                            test_key, sizeof(test_key),
                            derive_err, sizeof(derive_err))) {
        std::cout << "✅ Teste de derivação bem-sucedido! Primeiros 3 bytes da chave: "
                  << std::hex << std::setw(2) << std::setfill('0')
                  << (int)test_key[0] << " "
                  << (int)test_key[1] << " "
                  << (int)test_key[2] << std::dec << std::endl;
    } else {
        std::cerr << "❌ Teste de derivação falhou: " << derive_err << std::endl;
    }
    // ---------------------------------------------------------------------

    std::cout << "✅ TPM 2.0 Manager initialized successfully!" << std::endl;
    return true;
}

tpm2::TPMManager& getTPMManager() {
    return g_tpm_manager;
}

void shutdownTPM() {
    if (g_tpm_initialized) {
        std::cout << "🔒 Shutting down TPM 2.0 Manager..." << std::endl;
        g_tpm_manager.shutdown();
        g_tpm_initialized = false;
        std::cout << "✅ TPM 2.0 Manager shutdown complete." << std::endl;
    }
}

// ============================================================================
// deriveKeyFromPepper – implementação manual de HKDF-SHA256 (RFC 5869)
// ============================================================================
bool deriveKeyFromPepper(memorymanager::MemoryManagerThread* mm,
                         const uint8_t* salt, size_t salt_len,
                         uint8_t* out_key, size_t out_len,
                         char* out_error, size_t err_size) {
    (void)mm; // reservado para futura integração com a arena

    if (!g_tpm_initialized) {
        if (out_error && err_size > 0)
            Utils::safeCopyString(out_error, err_size, "TPM not initialized");
        return false;
    }

    if (!salt || salt_len == 0 || !out_key || out_len == 0) {
        if (out_error && err_size > 0)
            Utils::safeCopyString(out_error, err_size, "Invalid parameters (salt, buffer, or length)");
        return false;
    }

    // Obter pepper do TPM (cópia local)
    uint8_t pepper[64] = {0};
    char pepper_err[256] = {0};
    if (!g_tpm_manager.getPepper(pepper, sizeof(pepper), pepper_err, sizeof(pepper_err))) {
        if (out_error && err_size > 0)
            Utils::safeCopyString(out_error, err_size, "Failed to retrieve pepper from TPM");
        return false;
    }

    // Limite máximo para alocação na stack (evita heap)
    const size_t MAX_TEMP = 1024;
    if (out_len > MAX_TEMP) {
        if (out_error && err_size > 0)
            Utils::safeCopyString(out_error, err_size, "Requested key length exceeds maximum (1024)");
        OPENSSL_cleanse(pepper, sizeof(pepper));
        return false;
    }

    uint8_t temp_buf[MAX_TEMP];   // buffer temporário na stack

    // ------------------------------------------------------------
    // PASSO 1: EXTRACT – PRK = HMAC-SHA256(salt, pepper)
    // ------------------------------------------------------------
    const EVP_MD* md = EVP_sha256();
    uint8_t prk[32];
    unsigned int prk_len;
    if (!HMAC(md, salt, (int)salt_len, pepper, sizeof(pepper), prk, &prk_len)) {
        if (out_error && err_size > 0)
            Utils::safeCopyString(out_error, err_size, "HMAC extract failed");
        OPENSSL_cleanse(pepper, sizeof(pepper));
        return false;
    }
    if (prk_len != 32) {
        OPENSSL_cleanse(pepper, sizeof(pepper));
        OPENSSL_cleanse(prk, sizeof(prk));
        if (out_error && err_size > 0)
            Utils::safeCopyString(out_error, err_size, "Unexpected PRK length");
        return false;
    }

    // ------------------------------------------------------------
    // PASSO 2: EXPAND – gerar out_len bytes usando contador
    // ------------------------------------------------------------
    const char* info = "TPM pepper derived key - test";
    size_t info_len = strlen(info);

    uint8_t* out_pos = temp_buf;
    size_t remaining = out_len;
    uint8_t counter = 1;
    uint8_t previous[32] = {0}; // T(0) = empty
    size_t prev_len = 0;

    while (remaining > 0) {
        HMAC_CTX* ctx = HMAC_CTX_new();
        if (!ctx) {
            OPENSSL_cleanse(prk, sizeof(prk));
            OPENSSL_cleanse(pepper, sizeof(pepper));
            if (out_error && err_size > 0)
                Utils::safeCopyString(out_error, err_size, "HMAC_CTX_new failed");
            return false;
        }

        if (HMAC_Init_ex(ctx, prk, prk_len, md, NULL) != 1) {
            HMAC_CTX_free(ctx);
            OPENSSL_cleanse(prk, sizeof(prk));
            OPENSSL_cleanse(pepper, sizeof(pepper));
            if (out_error && err_size > 0)
                Utils::safeCopyString(out_error, err_size, "HMAC_Init_ex failed");
            return false;
        }

        // Adicionar previous (se houver)
        if (prev_len > 0) {
            if (HMAC_Update(ctx, previous, prev_len) != 1) {
                HMAC_CTX_free(ctx);
                OPENSSL_cleanse(prk, sizeof(prk));
                OPENSSL_cleanse(pepper, sizeof(pepper));
                if (out_error && err_size > 0)
                    Utils::safeCopyString(out_error, err_size, "HMAC_Update (previous) failed");
                return false;
            }
        }

        // Adicionar info
        if (HMAC_Update(ctx, (const uint8_t*)info, info_len) != 1) {
            HMAC_CTX_free(ctx);
            OPENSSL_cleanse(prk, sizeof(prk));
            OPENSSL_cleanse(pepper, sizeof(pepper));
            if (out_error && err_size > 0)
                Utils::safeCopyString(out_error, err_size, "HMAC_Update (info) failed");
            return false;
        }

        // Adicionar counter (1 byte)
        if (HMAC_Update(ctx, &counter, 1) != 1) {
            HMAC_CTX_free(ctx);
            OPENSSL_cleanse(prk, sizeof(prk));
            OPENSSL_cleanse(pepper, sizeof(pepper));
            if (out_error && err_size > 0)
                Utils::safeCopyString(out_error, err_size, "HMAC_Update (counter) failed");
            return false;
        }

        uint8_t block[32];
        unsigned int block_len;
        if (HMAC_Final(ctx, block, &block_len) != 1) {
            HMAC_CTX_free(ctx);
            OPENSSL_cleanse(prk, sizeof(prk));
            OPENSSL_cleanse(pepper, sizeof(pepper));
            if (out_error && err_size > 0)
                Utils::safeCopyString(out_error, err_size, "HMAC_Final failed");
            return false;
        }
        HMAC_CTX_free(ctx);

        if (block_len != 32) {
            OPENSSL_cleanse(prk, sizeof(prk));
            OPENSSL_cleanse(pepper, sizeof(pepper));
            OPENSSL_cleanse(block, sizeof(block));
            if (out_error && err_size > 0)
                Utils::safeCopyString(out_error, err_size, "Unexpected block length");
            return false;
        }

        // Copiar o que couber
        size_t to_copy = (remaining < 32) ? remaining : 32;
        memcpy(out_pos, block, to_copy);
        out_pos += to_copy;
        remaining -= to_copy;

        // Guardar este bloco como 'previous' para a próxima iteração
        memcpy(previous, block, 32);
        prev_len = 32;

        counter++;
        if (counter == 0) {
            // Overflow – impossível com out_len <= 1024 (máx 32 iterações)
            OPENSSL_cleanse(prk, sizeof(prk));
            OPENSSL_cleanse(pepper, sizeof(pepper));
            OPENSSL_cleanse(block, sizeof(block));
            if (out_error && err_size > 0)
                Utils::safeCopyString(out_error, err_size, "Counter overflow");
            return false;
        }
    }

    // Copiar resultado final para o buffer do chamador
    memcpy(out_key, temp_buf, out_len);

    // Limpeza rigorosa
    OPENSSL_cleanse(pepper, sizeof(pepper));
    OPENSSL_cleanse(prk, sizeof(prk));
    OPENSSL_cleanse(previous, sizeof(previous));
    OPENSSL_cleanse(temp_buf, out_len);

    return true;
}

} // namespace TPMStart