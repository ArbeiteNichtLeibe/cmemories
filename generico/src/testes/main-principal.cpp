// main.cpp - COM JSON HANDLER INTEGRADO
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#include <pwd.h>
#include <signal.h>

// ============================================================
// INCLUSÃO DOS HEADERS
// ============================================================
#include "../../memorymanager/include/memory_manager_thread.hpp"
#include "../../lerconfig/include/config.hpp"
#include "../../jsonhandler/include/json_handler.hpp"  // NOVO: JSON Handler

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
    "boas_vindas = Sistema de Controle de Memorandos versão 0.001 - ainda alfa\n"
    "template_file = /var/lib/memorandos/templates.bin\n";

// ============================================================
// VARIÁVEIS GLOBAIS
// ============================================================
static memorymanager::MemoryManagerThread g_memory_manager;
static volatile sig_atomic_t g_signal_received = 0;

// ============================================================
// FUNÇÕES AUXILIARES
// ============================================================

static bool isRoot() { 
    return geteuid() == 0; 
}

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
    } else {
        return false;
    }
    if (num_len == 0) return false;

    char num_str[32];
    size_t num_pos = 0;
    for (size_t i = 0; i < num_len && num_pos < sizeof(num_str) - 1; ++i) {
        if (value_str[i] >= '0' && value_str[i] <= '9') {
            num_str[num_pos++] = value_str[i];
        } else {
            return false;
        }
    }
    num_str[num_pos] = '\0';
    if (num_pos == 0) return false;

    char* endptr = nullptr;
    unsigned long long value = std::strtoull(num_str, &endptr, 10);
    if (endptr == num_str || *endptr != '\0') return false;
    if (value == 0) return false;

    if (value < MIN_GB) {
        gigabytes = MIN_GB;
        return true;
    } else if (value > MAX_GB) {
        gigabytes = MAX_GB;
        return true;
    } else {
        gigabytes = static_cast<uint64_t>(value);
        return true;
    }
}

static void printHelp(const char* prog) {
    (void)prog;
    std::cout << "Usage: servidorprincipal [--memory=XXGb]\n"
              << "  --memory=XXGb   Total memory to allocate (" << MIN_GB << "-" << MAX_GB << " Gb, default " << DEFAULT_GB << "Gb)\n"
              << "  --help          Show this help\n";
}

static bool createConfigDirectory() {
    struct stat st;
    if (stat(CONFIG_DIR, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        std::cerr << "❌ " << CONFIG_DIR << " is not a directory." << std::endl;
        return false;
    }
    if (mkdir(CONFIG_DIR, 0755) != 0) {
        std::cerr << "❌ mkdir " << CONFIG_DIR << ": " << strerror(errno) << std::endl;
        return false;
    }
    std::cout << "📁 Directory " << CONFIG_DIR << " created." << std::endl;
    return true;
}

static bool configFileExists() {
    struct stat st;
    return stat(CONFIG_FILE, &st) == 0;
}

static bool createDefaultConfig() {
    if (!createConfigDirectory()) return false;
    int fd = open(CONFIG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        std::cerr << "❌ open " << CONFIG_FILE << ": " << strerror(errno) << std::endl;
        return false;
    }
    size_t len = std::strlen(DEFAULT_CONFIG_CONTENT);
    ssize_t written = write(fd, DEFAULT_CONFIG_CONTENT, len);
    close(fd);
    if (written != static_cast<ssize_t>(len)) {
        std::cerr << "❌ Error writing to file." << std::endl;
        return false;
    }
    std::cout << "📄 Default config created at " << CONFIG_FILE << std::endl;
    return true;
}

static bool dropToUser(const char* username) {
    if (!username || username[0] == '\0') {
        std::cerr << "❌ Empty username." << std::endl;
        return false;
    }
    struct passwd* pw = getpwnam(username);
    if (!pw) {
        std::cerr << "❌ User '" << username << "' not found on system." << std::endl;
        return false;
    }
    if (setgid(pw->pw_gid) != 0) {
        std::cerr << "❌ setgid to " << username << " failed: " << strerror(errno) << std::endl;
        return false;
    }
    if (setuid(pw->pw_uid) != 0) {
        std::cerr << "❌ setuid to " << username << " failed: " << strerror(errno) << std::endl;
        return false;
    }
    std::cout << "✅ Privileges dropped to user: " << username << " (UID " << pw->pw_uid << ")" << std::endl;
    return true;
}

// ============================================================
// SIGNAL HANDLER
// ============================================================
static void signalHandler(int sig) {
    (void)sig;
    g_signal_received = 1;
    JSONHandler::stop();  // Para o JSON Handler
}

// ============================================================
// FUNÇÃO PARA TESTAR O MEMORY MANAGER (OPCIONAL)
// ============================================================
static void testMemoryAllocation(memorymanager::MemoryManagerThread& mm, uint64_t thread_id, uint32_t megabytes) {
    uint32_t start_block = 0u;
    void* start_addr = nullptr;
    void* end_addr = nullptr;
    char error_msg[256] = {0};
    
    std::cout << "🧪 Testing allocation for thread " << thread_id 
              << ": " << megabytes << "MB" << std::endl;
    
    if (mm.allocate(thread_id, megabytes, start_block, start_addr, end_addr, error_msg)) {
        std::cout << "   ✅ Allocated " << megabytes << "MB at block " 
                  << start_block << " (addr: " << start_addr << ")" << std::endl;
        
        if (mm.free(thread_id, error_msg)) {
            std::cout << "   ✅ Freed successfully" << std::endl;
        } else {
            std::cout << "   ❌ Failed to free: " << error_msg << std::endl;
        }
    } else {
        std::cout << "   ❌ Allocation failed: " << error_msg << std::endl;
    }
}

// ============================================================
// MAIN - PONTO DE ENTRADA
// ============================================================
int main(int argc, char* argv[]) {
    // ============================================================
    // 1. CHECK ROOT
    // ============================================================
    if (!isRoot()) {
        std::cerr << "❌ Run as root." << std::endl;
        return 1;
    }

    // ============================================================
    // 2. PROCESS ARGUMENTS
    // ============================================================
    uint64_t requested_gb = DEFAULT_GB;
    bool memory_set = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printHelp(argv[0]);
            return 0;
        }
        if (parseMemoryArg(argv[i], requested_gb)) {
            memory_set = true;
        }
    }
    if (!memory_set) {
        std::cout << "INFO: Using default: " << DEFAULT_GB << "Gb" << std::endl;
    }
    
    std::cout << "📊 Requested memory: " << requested_gb << " GB" << std::endl;

    // ============================================================
    // 3. CONFIGURATION FILE
    // ============================================================
    if (!configFileExists()) {
        std::cout << "⚠️ Creating default configuration..." << std::endl;
        if (!createDefaultConfig()) {
            std::cerr << "❌ Failed to create config file." << std::endl;
            return 1;
        }
    }

    // ============================================================
    // 4. GET SERVICE USER INFO
    // ============================================================
    char workeruser_buf[64] = {0};
    FILE* fp = fopen(CONFIG_FILE, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            size_t len = std::strlen(line);
            if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
            if (len > 0 && line[len-1] == '\r') line[len-1] = '\0';
            
            char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (std::strncmp(p, "workeruser", 10) == 0) {
                char* eq = std::strchr(p, '=');
                if (eq) {
                    char* val = eq + 1;
                    while (*val == ' ' || *val == '\t') val++;
                    char* end = val + std::strlen(val) - 1;
                    while (end > val && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end--;
                    if (end > val) {
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

    if (workeruser_buf[0] == '\0') {
        std::cerr << "❌ Key 'workeruser' not found in config." << std::endl;
        return 1;
    }
    std::cout << "👤 workeruser configured: " << workeruser_buf << std::endl;

    struct passwd* pw = getpwnam(workeruser_buf);
    if (!pw) {
        std::cerr << "❌ User '" << workeruser_buf << "' not found." << std::endl;
        return 1;
    }

    // ============================================================
    // 5. CREATE SOCKET DIRECTORY
    // ============================================================
    const char* socket_dir = "/home/memorandos";
    struct stat st;
    if (stat(socket_dir, &st) != 0) {
        if (mkdir(socket_dir, 0755) != 0) {
            std::cerr << "❌ mkdir " << socket_dir << ": " << strerror(errno) << std::endl;
            return 1;
        }
        std::cout << "📁 Directory " << socket_dir << " created." << std::endl;
    } else if (!S_ISDIR(st.st_mode)) {
        std::cerr << "❌ " << socket_dir << " is not a directory." << std::endl;
        return 1;
    }

    if (chown(socket_dir, pw->pw_uid, pw->pw_gid) != 0) {
        std::cerr << "❌ chown " << socket_dir << " to " << workeruser_buf
                  << " failed: " << strerror(errno) << std::endl;
        return 1;
    }
    std::cout << "✅ Directory owner set to " << workeruser_buf << std::endl;

    // ============================================================
    // 6. DROP PRIVILEGES
    // ============================================================
    if (!dropToUser(workeruser_buf)) {
        std::cerr << "❌ Failed to drop privileges." << std::endl;
        return 1;
    }

    // ============================================================
    // 7. INITIALIZE MEMORY MANAGER
    // ============================================================
    std::cout << "📊 Initializing MemoryManagerThread with " 
              << requested_gb << " GB..." << std::endl;
    
    if (!g_memory_manager.init(static_cast<uint32_t>(requested_gb))) {
        std::cerr << "❌ MemoryManagerThread::init() failed!" << std::endl;
        return 1;
    }
    
    std::cout << "✅ MemoryManagerThread initialized successfully!" << std::endl;

    // ============================================================
    // 8. INJETA MEMORY MANAGER NO CONFIG
    // ============================================================
    LerConfig::Config::setMemoryManager(&g_memory_manager);
    std::cout << "✅ MemoryManager injected into Config" << std::endl;

    // ============================================================
    // 9. LOAD CONFIGURATIONS
    // ============================================================
    LerConfig::Config& config = LerConfig::Config::getInstance();
    if (!config.carregar(CONFIG_FILE)) {
        std::cerr << "❌ Failed to load configurations." << std::endl;
        LerConfig::Config::setMemoryManager(nullptr);
        g_memory_manager.shutdown();
        return 1;
    }

    // ============================================================
    // 10. TESTE DO CONFIG
    // ============================================================
    std::cout << "\n========================================" << std::endl;
    std::cout << "📋 TESTE DE CONFIGURAÇÃO" << std::endl;
    std::cout << "========================================" << std::endl;
    
    const char* mensagem = config.getString("boas_vindas", "Mensagem padrão não encontrada!");
    std::cout << "💬 Mensagem de boas-vindas: " << mensagem << std::endl;
    
    std::cout << "\n📋 OUTRAS CONFIGURAÇÕES:" << std::endl;
    std::cout << "   Ragnarok: " << config.getString("ragnarok", "N/A") << std::endl;
    std::cout << "   Memory: " << config.getInt("memory", 0) << " GB" << std::endl;
    std::cout << "   DB Host: " << config.getString("db_host", "N/A") << std::endl;
    std::cout << "   DB Port: " << config.getInt("db_port", 0) << std::endl;
    std::cout << "   Auth Enabled: " << (config.getBool("auth_enabled", false) ? "✅ Sim" : "❌ Não") << std::endl;
    std::cout << "   Rate Limit: " << config.getInt("rate_limit", 0) << std::endl;
    std::cout << "   Template File: " << config.getString("template_file", "N/A") << std::endl;
    std::cout << "========================================" << std::endl;

    // ============================================================
    // 11. TESTE RÁPIDO DO MEMORY MANAGER
    // ============================================================
    std::cout << "\n🧪 Running quick memory test..." << std::endl;
    testMemoryAllocation(g_memory_manager, 1, 10);
    testMemoryAllocation(g_memory_manager, 2, 50);
    testMemoryAllocation(g_memory_manager, 3, 100);
    std::cout << "✅ Memory test completed.\n" << std::endl;

    // ============================================================
    // 12. INICIALIZAR JSON HANDLER (NOVO)
    // ============================================================
    std::cout << "📊 Initializing JSON Handler..." << std::endl;
    char json_error[512] = {0};
    
    if (!JSONHandler::initialize(&g_memory_manager, json_error, sizeof(json_error))) {
        std::cerr << "❌ JSON Handler initialization failed: " << json_error << std::endl;
        LerConfig::Config::setMemoryManager(nullptr);
        g_memory_manager.shutdown();
        return 1;
    }
    
    std::cout << "✅ JSON Handler initialized." << std::endl;
    std::cout << "📊 Templates loaded: " << (JSONHandler::isLoaded() ? "Yes" : "No") << std::endl;

    // ============================================================
    // 13. SETUP SIGNALS
    // ============================================================
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ============================================================
    // 14. MOSTRA INFORMAÇÕES DO SISTEMA
    // ============================================================
    uint32_t total_blocks = 0u;
    size_t block_size = 0u;
    uint32_t active_loans = 0u;
    uint64_t allocated_bytes = 0u;
    
    if (g_memory_manager.get_info(total_blocks, block_size, active_loans, allocated_bytes)) {
        std::cout << "\n📊 Memory Manager Info:\n"
                  << "   Total Blocks: " << total_blocks << "\n"
                  << "   Block Size: " << (block_size / (1024*1024)) << " MB\n"
                  << "   Active Loans: " << active_loans << "\n"
                  << "   Allocated: " << (allocated_bytes / (1024*1024)) << " MB\n";
    }
    
    std::cout << "\n🔄 Sistema pronto. Pressione Ctrl+C para encerrar.\n";

    // ============================================================
    // 15. EXECUTAR JSON HANDLER (LOOP PRINCIPAL)
    // ============================================================
    char run_error[512] = {0};
    if (!JSONHandler::run(run_error, sizeof(run_error))) {
        std::cerr << "⚠️ JSON Handler run stopped: " << run_error << std::endl;
    }

    // ============================================================
    // 16. SHUTDOWN
    // ============================================================
    std::cout << "\n🛑 Shutting down..." << std::endl;
    
    // Remove referência do Config para o MemoryManager
    LerConfig::Config::setMemoryManager(nullptr);
    
    // Shutdown do MemoryManager
    g_memory_manager.shutdown();
    std::cout << "✅ MemoryManagerThread shutdown complete." << std::endl;

    return 0;
}