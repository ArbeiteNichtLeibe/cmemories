#include "../include/tpm2_info.hpp"
#include "../../uteis/include/uteis.hpp"

#include <iostream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <openssl/evp.h>
#include <tss2/tss2_esys.h>

namespace tpm2 {

constexpr TPM2_HANDLE PEPPER_NV_INDEX = 0x01800001;
constexpr size_t PEPPER_SIZE = 64; // SHA-512 (64 bytes)

static void set_error(char* out_error, size_t err_size, const char* msg) {
    if (out_error && err_size > 0) {
        Utils::safeCopyString(out_error, err_size, msg);
    }
}

// Auxiliar para imprimir os 3 primeiros bytes em formato Hexadecimal
static void print_first_3_bytes(const char* label, const uint8_t* buffer) {
    std::cout << "   📌 [TPM] " << label << ": "
              << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(buffer[0]) << " "
              << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(buffer[1]) << " "
              << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(buffer[2])
              << std::dec << std::endl;
}

Session::Session() {
    TSS2_RC rc = Esys_Initialize(&esys_ctx_, nullptr, nullptr);
    valid_ = (rc == TSS2_RC_SUCCESS);
}

Session::~Session() {
    if (esys_ctx_) {
        Esys_Finalize(&esys_ctx_);
        esys_ctx_ = nullptr;
    }
    valid_ = false;
}

Info get_info(Session& session, char* out_error, size_t err_size) {
    Info info{};
    if (!session.is_valid()) {
        set_error(out_error, err_size, "Sessao TPM invalida");
        return info;
    }
    std::strncpy(info.manufacturer, "TPM2_Vendor", sizeof(info.manufacturer) - 1);
    std::strncpy(info.vendor_string, "Generic TPM2 Device", sizeof(info.vendor_string) - 1);
    info.success = true;
    return info;
}

// -----------------------------------------------------------------------------
// FUNÇÕES AUXILIARES
// -----------------------------------------------------------------------------

static bool read_existing_pepper(ESYS_CONTEXT* ctx, uint8_t* out_pepper_64b) {
    ESYS_TR nvHandle = ESYS_TR_NONE;

    TSS2_RC rc = Esys_TR_FromTPMPublic(ctx, PEPPER_NV_INDEX, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &nvHandle);
    if (rc != TSS2_RC_SUCCESS) {
        return false;
    }

    TPM2B_MAX_NV_BUFFER* nvData = nullptr;
    rc = Esys_NV_Read(ctx, nvHandle, nvHandle, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE, PEPPER_SIZE, 0, &nvData);
    
    bool success = false;
    if (rc == TSS2_RC_SUCCESS && nvData && nvData->size == PEPPER_SIZE) {
        std::memcpy(out_pepper_64b, nvData->buffer, PEPPER_SIZE);
        success = true;
    }

    if (nvData) {
        Esys_Free(nvData);
    }
    return success;
}

static bool generate_tpm_bytes(ESYS_CONTEXT* ctx, uint8_t* out_buf, size_t count) {
    size_t offset = 0;
    while (offset < count) {
        uint16_t req = static_cast<uint16_t>(std::min(count - offset, static_cast<size_t>(32)));
        TPM2B_DIGEST* random_bytes = nullptr;
        
        TSS2_RC rc = Esys_GetRandom(ctx, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, req, &random_bytes);
        if (rc != TSS2_RC_SUCCESS || !random_bytes) {
            return false;
        }

        std::memcpy(out_buf + offset, random_bytes->buffer, random_bytes->size);
        offset += random_bytes->size;
        Esys_Free(random_bytes);
    }
    return true;
}

static void compute_sha512(const uint8_t* input, size_t len, uint8_t* output_64bytes) {
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha512(), nullptr);
    EVP_DigestUpdate(mdctx, input, len);
    EVP_DigestFinal_ex(mdctx, output_64bytes, nullptr);
    EVP_MD_CTX_free(mdctx);
}

// -----------------------------------------------------------------------------
// FLUXO PRINCIPAL DE LEITURA OU GERAÇÃO + LOG NO TERMINAL
// -----------------------------------------------------------------------------
bool obtain_or_create_pepper(Session& session, uint8_t* out_pepper_64b, char* out_error, size_t err_size) {
    if (!session.is_valid()) {
        set_error(out_error, err_size, "Sessao TPM invalida");
        std::cerr << "   ❌ [TPM ERROR] Sessao TPM invalida." << std::endl;
        return false;
    }

    ESYS_CONTEXT* ctx = static_cast<ESYS_CONTEXT*>(session.get_esys());

    // PASSO 1: Tenta ler o Pepper existente na NVRAM
    if (read_existing_pepper(ctx, out_pepper_64b)) {
        std::cout << "   🔍 [TPM STATUS] Chave encontrada na NVRAM (0x01800001)! Estado: LIDO" << std::endl;
        print_first_3_bytes("Primeiros 3 bytes lidos do chip", out_pepper_64b);
        return true;
    }

    // PASSO 2: Não encontrada -> Gerar a palavra aleatória de 1025 bytes
    std::cout << "   🔍 [TPM STATUS] Chave nao encontrada na NVRAM. Estado: GERANDO" << std::endl;

    std::vector<uint8_t> raw_1025_bytes(1025);
    if (!generate_tpm_bytes(ctx, raw_1025_bytes.data(), 1025)) {
        set_error(out_error, err_size, "Falha ao gerar bytes aleatorios no TPM");
        std::cerr << "   ❌ [TPM ERROR] Falha ao gerar 1025 bytes aleatorios." << std::endl;
        return false;
    }
    
    print_first_3_bytes("Primeiros 3 bytes dos 1025 gerados pelo hardware", raw_1025_bytes.data());

    // PASSO 3: Calcular SHA-512
    uint8_t sha512_digest[PEPPER_SIZE];
    compute_sha512(raw_1025_bytes.data(), 1025, sha512_digest);
    print_first_3_bytes("Primeiros 3 bytes do Hash SHA-512 calculado", sha512_digest);

    // PASSO 4: Reservar NVRAM
    ESYS_TR nvHandle = ESYS_TR_NONE;
    TPM2B_AUTH nvAuth = {};
    TPM2B_NV_PUBLIC publicInfo = {};
    publicInfo.nvPublic.nvIndex = PEPPER_NV_INDEX;
    publicInfo.nvPublic.nameAlg = TPM2_ALG_SHA256;
    publicInfo.nvPublic.attributes = (TPMA_NV_OWNERWRITE | TPMA_NV_AUTHWRITE | 
                                      TPMA_NV_OWNERREAD  | TPMA_NV_AUTHREAD);
    publicInfo.nvPublic.dataSize = PEPPER_SIZE;

    TSS2_RC rc = Esys_NV_DefineSpace(ctx, ESYS_TR_RH_OWNER, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
                                     &nvAuth, &publicInfo, &nvHandle);
    if (rc != TSS2_RC_SUCCESS) {
        set_error(out_error, err_size, "Falha ao alocar indice na NVRAM do TPM");
        std::cerr << "   ❌ [TPM ERROR] Falha ao definir espaco na NVRAM." << std::endl;
        return false;
    }

    // PASSO 5: Escrever na NVRAM
    TPM2B_MAX_NV_BUFFER nvData = {};
    nvData.size = PEPPER_SIZE;
    std::memcpy(nvData.buffer, sha512_digest, PEPPER_SIZE);

    rc = Esys_NV_Write(ctx, nvHandle, nvHandle, ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE, &nvData, 0);
    if (rc != TSS2_RC_SUCCESS) {
        set_error(out_error, err_size, "Falha ao gravar Pepper na NVRAM");
        std::cerr << "   ❌ [TPM ERROR] Falha ao gravar dados na NVRAM." << std::endl;
        return false;
    }

    std::memcpy(out_pepper_64b, sha512_digest, PEPPER_SIZE);
    std::cout << "   ✅ [TPM STATUS] Pepper gravado no chip! Estado: GRAVADO" << std::endl;
    print_first_3_bytes("Primeiros 3 bytes gravados na NVRAM", out_pepper_64b);
    
    return true;
}

// -----------------------------------------------------------------------------
// STUBS DE COMPATIBILIDADE
// -----------------------------------------------------------------------------
bool generate_key(Session&, Key& key, const char* name, char*, size_t) {
    if (name) Utils::safeCopyString(key.name, sizeof(key.name), name);
    key.loaded = true;
    return true;
}

bool load_keys(Session&, Key& key, const char*, const char*, char*, size_t) {
    key.loaded = true;
    return true;
}

bool save_keys(const Key&, const char*, const char*, char*, size_t) {
    return true;
}

void unload_key(Session&, Key& key) {
    key.loaded = false;
}

} // namespace tpm2