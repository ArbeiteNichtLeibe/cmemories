// main.cpp - SERVIDOR PRINCIPAL
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#include <pwd.h>
#include <grp.h>          // <-- ADICIONADO para getgrnam()
#include "json_start.hpp" 
#include "key_generator.hpp"

// ============================================================
// INCLUSÃO DOS HEADERS
// ============================================================
#include "../../memorymanager/include/memory_manager_thread.hpp"
#include "../../lerconfig/include/config.hpp"
#include "../include/start_tpm.hpp"
#include "shutdown_manager.hpp"

// ============================================================
// CONSTANTES
// ============================================================
static const uint64_t MIN_GB = 5;
static const uint64_t MAX_GB = 30;
static const uint64_t DEFAULT_GB = 5;
static const char* CONFIG_DIR  = "/etc/memorandos";
static const char* CONFIG_FILE = "/etc/memorandos/config.conf";
static const char* DEFAULT_CONFIG_CONTENT =
    "# Configuração padrão do sistema\n"
    "ragnarok = desenvolvimento\n"
    "memory = 20\n"
    "workeruser = andre\n"
    "db_host = localhost\n"
    "db_port = 5432\n"
    "db_name = memorandos\n"
    "db_user = postgres\n"
    "db_password = \n"
    "api_prefix = /api/v1\n"
    "auth_enabled = true\n"
    "rate_limit = 100\n"
    "http_docs = /home/memorandos/www\n"
    "boas_vindas = Sistema de Controle de Memorandos versão 0.001 - ainda alfa\n"
    "template_file = /home/andre/memorandos/templates.bin\n";

// ============================================================
// VARIÁVEIS GLOBAIS
// ============================================================
static memorymanager::MemoryManagerThread g_memory_manager;

// ============================================================
// FUNÇÕES AUXILIARES
// ============================================================
static bool isRoot() { return geteuid() == 0; }

static bool parseMemoryArg(const char* arg, uint64_t& gigabytes) {
    if (!arg) return false;
    const char* prefix = "--memory=";
    size_t prefix_len = std::strlen(prefix);
    if (std::strncmp(arg, prefix, prefix_len) != 0) return false;

    const char* value_str = arg + prefix_len;
    size_t value_len = std::strlen(value_str);
    if (value_len < 3) return false;

    char last1 = value_str[value_len - 1];
    char last2 = value_str[value_len - 2];
    size_t num_len = value_len;
    if ((last1 == 'b' || last1 == 'B') && (last2 == 'g' || last2 == 'G')) {
        num_len = value_len - 2;
    } else return false;

    char num_str[32];
    size_t num_pos = 0;
    for (size_t i = 0; i < num_len && num_pos < sizeof(num_str) - 1; ++i) {
        if (value_str[i] >= '0' && value_str[i] <= '9') num_str[num_pos++] = value_str[i];
        else return false;
    }
    num_str[num_pos] = '\0';
    if (num_pos == 0) return false;

    char* endptr = nullptr;
    unsigned long long value = std::strtoull(num_str, &endptr, 10);
    if (endptr == num_str || *endptr != '\0') return false;
    
    if (value < MIN_GB) gigabytes = MIN_GB;
    else if (value > MAX_GB) gigabytes = MAX_GB;
    else gigabytes = static_cast<uint64_t>(value);
    return true;
}

static void printHelp(const char* prog) {
    std::cout << "Usage: " << prog << " [--memory=XXGb] [--accepttp2mchanges=yes]\n"
              << "  --memory=XXGb              Total memory to allocate (" << MIN_GB << "-" << MAX_GB << " Gb)\n"
              << "  --accepttp2mchanges=yes    Mandatory: confirm TPM NVRAM creation\n"
              << "  --help                     Show this help\n";
}

static bool createConfigDirectory() {
    struct stat st;
    if (stat(CONFIG_DIR, &st) == 0) return S_ISDIR(st.st_mode);
    return (mkdir(CONFIG_DIR, 0755) == 0);
}

static bool configFileExists() {
    struct stat st;
    return stat(CONFIG_FILE, &st) == 0;
}

static bool createDefaultConfig() {
    if (!createConfigDirectory()) return false;
    int fd = open(CONFIG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return false;
    write(fd, DEFAULT_CONFIG_CONTENT, std::strlen(DEFAULT_CONFIG_CONTENT));
    close(fd);
    return true;
}

static bool dropToUser(const char* username) {
    struct passwd* pw = getpwnam(username);
    if (!pw) return false;
    if (setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0) return false;
    return true;
}

// ============================================================
// MAIN - PONTO DE ENTRADA
// ============================================================
int main(int argc, char* argv[]) {
    ShutdownManager::setupHandlers();
    ShutdownManager::startMonitor();

    if (!isRoot()) { 
        std::cerr << "❌ Run as root." << std::endl; 
        return 1; 
    }

    uint64_t requested_gb = DEFAULT_GB;
    bool accept_tpm_changes = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) { 
            printHelp(argv[0]); 
            return 0; 
        }
        parseMemoryArg(argv[i], requested_gb);
        if (std::strcmp(argv[i], "--accepttp2mchanges=yes") == 0) {
            accept_tpm_changes = true;
        }
    }

    if (!configFileExists()) {
        if (!createDefaultConfig()) { 
            std::cerr << "❌ Failed to create config file." << std::endl; 
            return 1; 
        }
    }

    // Leitura do workeruser no arquivo de configuração
    char workeruser_buf[64] = "andre"; 
    FILE* fp = fopen(CONFIG_FILE, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (std::strncmp(p, "workeruser", 10) == 0) {
                char* eq = std::strchr(p, '=');
                if (eq) {
                    char* val = eq + 1;
                    while (*val == ' ' || *val == '\t') val++;
                    char* end = val + std::strlen(val) - 1;
                    while (end > val && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end--;
                    if (end >= val) {
                        size_t val_len = end - val + 1;
                        if (val_len < sizeof(workeruser_buf)) {
                            std::memcpy(workeruser_buf, val, val_len);
                            workeruser_buf[val_len] = '\0';
                        }
                    }
                }
                break;
            }
        }
        fclose(fp);
    }

    // Criação do diretório de sockets/www se necessário e alteração de dono
    const char* socket_dir = "/home/memorandos";
    struct stat st;
    if (stat(socket_dir, &st) != 0) {
        mkdir(socket_dir, 0755);
    }
    struct passwd* pw = getpwnam(workeruser_buf);
    if (pw) {
        chown(socket_dir, pw->pw_uid, pw->pw_gid);
    }

    // ============================================================
    // 1. INICIALIZAR MEMORY MANAGER (ANTES DO DROP)
    // ============================================================
    std::cout << "📊 Initializing MemoryManagerThread with " << requested_gb << " GB..." << std::endl;
    if (!g_memory_manager.init(static_cast<uint32_t>(requested_gb))) {
        std::cerr << "❌ MemoryManagerThread::init() failed!" << std::endl;
        return 1;
    }

    // 2. Inject Memory Manager and Load Configurations
    LerConfig::Config::setMemoryManager(&g_memory_manager);
    LerConfig::Config& config = LerConfig::Config::getInstance();
    if (!config.carregar(CONFIG_FILE)) {
        std::cerr << "❌ Failed to load configurations." << std::endl;
        ShutdownManager::shutdownSystem(&g_memory_manager);
        return 1;
    }
    std::cout << "✅ Configurations loaded successfully." << std::endl;

    // 3. Verificação obrigatória do TPM
    if (!accept_tpm_changes) {
        std::cerr << "\n❌ É obrigatório confirmar as alterações no TPM com o argumento:\n"
                  << "   --accepttp2mchanges=yes\n\n" << std::endl;
        ShutdownManager::shutdownSystem(&g_memory_manager);
        return 1;
    }

    // 4. INICIALIZAR TPM (ANTES DO DROP)
    if (!TPMStart::initTPM(&g_memory_manager)) {
        ShutdownManager::shutdownSystem(&g_memory_manager);
        return 1;
    }

    // 5. CRIAR DIRETÓRIO PARA CHAVES (/run/memorandos)
    const char* key_dir = "/run/memorandos";
    struct stat st_dir;
    if (stat(key_dir, &st_dir) != 0) {
        if (mkdir(key_dir, 0755) != 0) {
            std::cerr << "❌ Failed to create " << key_dir << ": " << strerror(errno) << std::endl;
            ShutdownManager::shutdownSystem(&g_memory_manager);
            return 1;
        }
    }
    // Ajusta dono/grupo para www-data (opcional, mas recomendado)
    struct group* grp = getgrnam("www-data");
    if (grp) {
        chown(key_dir, -1, grp->gr_gid);
    }
    chmod(key_dir, 0755);

    // 6. GERAR CHAVES (COMO ROOT, COM TPM E MEMORY MANAGER)
    char errBuf[256] = {0};
    tpm2::TPMManager& tpm_manager = TPMStart::getTPMManager();
    if (!KeyGenerator::generateKeys(&g_memory_manager, &tpm_manager, errBuf, sizeof(errBuf))) {
        std::cerr << "❌ Key generation failed: " << errBuf << std::endl;
        ShutdownManager::shutdownSystem(&g_memory_manager);
        return 1;
    }
    std::cout << "✅ JWT key saved to /run/memorandos/jwt_chave.conf\n";
    std::cout << "✅ Password key saved to /run/memorandos/discreto.conf\n";

    // 7. DROP PARA USUÁRIO NÃO PRIVILEGIADO
    if (!dropToUser(workeruser_buf)) { 
        std::cerr << "❌ Failed to drop privileges." << std::endl; 
        ShutdownManager::shutdownSystem(&g_memory_manager);
        return 1; 
    }

    // 8. INICIALIZAR FAKEREDIS (AGORA COMO USUÁRIO)
    FakeRedis::FakeRedis& redis = FakeRedis::FakeRedis::getInstance();
    if (!redis.initialize(&g_memory_manager, 0, errBuf, sizeof(errBuf))) {
        std::cerr << "❌ FakeRedis::initialize failed: " << errBuf << std::endl;
        ShutdownManager::shutdownSystem(&g_memory_manager);
        return 1;
    }
    std::cout << "✅ FakeRedis initialized successfully." << std::endl;

    // 9. INICIALIZAR JSONSTART & HTTPSERVER (SEM GERAR CHAVES)
    if (!JSONStart::init(&g_memory_manager, &redis, false, errBuf, sizeof(errBuf))) {
        std::cerr << "❌ JSONStart::init failed: " << errBuf << std::endl;
        ShutdownManager::shutdownSystem(&g_memory_manager);
        return 1;
    }

    // Main Loop
    std::cout << "\n🔄 Sistema rodando. Pressione Ctrl+C para encerrar.\n";
    while (!ShutdownManager::g_should_stop.load()) {
        sleep(1);
    }

    ShutdownManager::shutdownSystem(&g_memory_manager);
    return 0;
}