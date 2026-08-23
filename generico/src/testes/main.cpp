// main.cpp
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#include <pwd.h>

// ============================================================
// INCLUSÃO DOS HEADERS
// ============================================================
#include "../../memorymanager/include/memory_manager_thread.hpp"
#include "../../lerconfig/include/config.hpp"
#include "../../jsonhandler/include/template_loader.hpp"

// ============================================================
// CONSTANTES
// ============================================================
static const char* CONFIG_FILE = "/etc/memorandos/config.conf";
static memorymanager::MemoryManagerThread g_memory_manager;

static bool isRoot() { return geteuid() == 0; }

// ============================================================
// FUNÇÃO PARA CRIAR CONFIG PADRÃO
// ============================================================
static void createDefaultConfig() {
    mkdir("/etc/memorandos", 0755);
    int fd = open(CONFIG_FILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd != -1) {
        const char* content = 
            "memory = 5\n"
            "workeruser = andre\n"
            "template_file = /tmp/templates.bin\n";
        write(fd, content, strlen(content));
        close(fd);
    }
}

// ============================================================
// FUNÇÃO PARA FAZER DOWNGRADE
// ============================================================
static bool dropToUser(const char* username) {
    struct passwd* pw = getpwnam(username);
    if (!pw) {
        std::cerr << "❌ User '" << username << "' not found." << std::endl;
        return false;
    }
    if (setgid(pw->pw_gid) != 0) {
        std::cerr << "❌ setgid failed: " << strerror(errno) << std::endl;
        return false;
    }
    if (setuid(pw->pw_uid) != 0) {
        std::cerr << "❌ setuid failed: " << strerror(errno) << std::endl;
        return false;
    }
    std::cout << "✅ Dropped to user: " << username << std::endl;
    return true;
}

// ============================================================
// FUNÇÃO PARA LER WORKERUSER DO CONFIG
// ============================================================
static void readWorkerUser(char* buffer, size_t size) {
    FILE* fp = fopen(CONFIG_FILE, "r");
    if (!fp) return;
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "workeruser", 10) == 0) {
            char* eq = strchr(p, '=');
            if (eq) {
                char* val = eq + 1;
                while (*val == ' ' || *val == '\t') val++;
                char* end = val + strlen(val) - 1;
                while (end > val && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end--;
                if (end > val) {
                    size_t len = end - val + 1;
                    if (len < size) {
                        memcpy(buffer, val, len);
                        buffer[len] = '\0';
                    }
                }
            }
            break;
        }
    }
    fclose(fp);
}

// ============================================================
// MAIN
// ============================================================
int main(void) {
    std::cout << "\n🧪 TESTE DO TEMPLATE_LOADER" << std::endl;
    std::cout << "=============================" << std::endl;

    // ============================================================
    // 1. VERIFICA ROOT
    // ============================================================
    if (!isRoot()) {
        std::cerr << "❌ Executar como root." << std::endl;
        return 1;
    }

    // ============================================================
    // 2. CRIA CONFIG SE NÃO EXISTIR
    // ============================================================
    struct stat st;
    if (stat(CONFIG_FILE, &st) != 0) {
        createDefaultConfig();
    }

    // ============================================================
    // 3. LÊ WORKERUSER
    // ============================================================
    char workeruser[64] = {0};
    readWorkerUser(workeruser, sizeof(workeruser));
    if (workeruser[0] == '\0') {
        std::cerr << "❌ workeruser not found in config." << std::endl;
        return 1;
    }
    std::cout << "👤 Worker user: " << workeruser << std::endl;

    // ============================================================
    // 4. FAZ DOWNGRADE
    // ============================================================
    if (!dropToUser(workeruser)) {
        return 1;
    }

    // ============================================================
    // 5. INICIALIZA MEMORY MANAGER
    // ============================================================
    if (!g_memory_manager.init(5)) {
        std::cerr << "❌ MemoryManager init failed." << std::endl;
        return 1;
    }
    std::cout << "✅ MemoryManager initialized." << std::endl;

    // ============================================================
    // 6. INJETA MEMORY MANAGER NO TEMPLATE_LOADER
    // ============================================================
    auto& loader = JSONHandler::TemplateLoader::getInstance();
    loader.setMemoryManager(&g_memory_manager);
    std::cout << "✅ MemoryManager injected." << std::endl;

    // ============================================================
    // 7. CARREGA CONFIG
    // ============================================================
    LerConfig::Config::setMemoryManager(&g_memory_manager);
    LerConfig::Config& config = LerConfig::Config::getInstance();
    if (!config.carregar(CONFIG_FILE)) {
        std::cerr << "❌ Failed to load config." << std::endl;
        return 1;
    }
    std::cout << "✅ Config loaded." << std::endl;

    // ============================================================
    // 8. TESTA O TEMPLATE_LOADER
    // ============================================================
    char err[256] = {0};
    bool result = loader.loadTemplates(err, sizeof(err));

    if (result) {
        std::cout << "\n✅ TEMPLATE_LOADER FUNCIONA!" << std::endl;
        std::cout << "   Arquivo: " << loader.getTemplateFilePath() << std::endl;
        std::cout << "   Tamanho: " << loader.getTemplateMapSize() << " bytes" << std::endl;
        std::cout << "   Endereço: " << (void*)loader.getTemplateMap() << std::endl;
    } else {
        std::cout << "\n❌ TEMPLATE_LOADER FALHOU!" << std::endl;
        std::cout << "   Erro: " << err << std::endl;
        return 1;
    }

    // ============================================================
    // 9. SHUTDOWN
    // ============================================================
    g_memory_manager.shutdown();
    std::cout << "\n✅ Teste concluído." << std::endl;

    return 0;
}