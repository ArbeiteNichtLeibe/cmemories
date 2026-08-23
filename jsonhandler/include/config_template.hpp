#ifndef CONFIG_TEMPLATE_HPP
#define CONFIG_TEMPLATE_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace JSONWorker {

struct TemplateHeader {
    uint32_t magic;        // 0xDEADBEEF
    uint32_t count;        // max 100
};

struct TemplateEntry {
    char nome[14];
    uint32_t offset;
    uint32_t tamanho;
};

constexpr uint32_t MAX_TEMPLATES = 100;
constexpr uint32_t MAGIC_START = 0xDEADBEEF;
constexpr uint32_t MAGIC_END   = 0xFEEDFACE;

bool start_template(void* memory, size_t memorySize, char* outError, size_t errSize);

bool consultar_template(const void* memory, size_t memorySize,
                        const char* nome,
                        char* outBuffer, size_t bufferSize,
                        char* outError, size_t errSize);

bool gravar_template(void* memory, size_t memorySize,
                     const char* nome, const char* conteudo,
                     char* outError, size_t errSize);

bool remover_template(void* memory, size_t memorySize,
                      const char* nome,
                      char* outError, size_t errSize);

bool verify_template_memory(const void* memory, size_t memorySize);

} // namespace JSONWorker

#endif