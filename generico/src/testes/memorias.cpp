// main.cpp
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <random>
#include <vector>
#include "../../memorymanager/include/memorymanager.hpp"

static const uint64_t MIN_GB = 5;
static const uint64_t MAX_GB = 20;
static const uint64_t DEFAULT_GB = 5;

struct Allocation {
    uint64_t thread_id;
    uint32_t start_block;
    uint32_t num_blocks;
};

// -------------------------------------------------------------------
//  Funções de Ajuda e Validação
// -------------------------------------------------------------------
static bool parse_memory_arg(const char* arg, uint64_t& gigabytes) {
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

static void print_help(const char* prog) {
    std::cout << "Uso: " << prog << " [--memory=XXGb]\n"
              << "  --memory=XXGb   Memória total a alocar (" << MIN_GB << "-" << MAX_GB << " Gb)\n";
}

static bool fill_and_verify(uint32_t start_block, uint32_t num_blocks, uint8_t pattern) {
    void* start_addr = nullptr;
    void* end_addr = nullptr;
    int ret = memorymanager::memory_manager_get_address(start_block, num_blocks, start_addr, end_addr);
    if (ret != memorymanager::MANAGER_OK) {
        return false;
    }

    if (!start_addr || !end_addr || start_addr >= end_addr) {
        std::cerr << "[ALERTA] Endereços retornados inválidos ou nulos!\n";
        return false;
    }

    uint8_t* ptr = static_cast<uint8_t*>(start_addr);
    size_t size = static_cast<char*>(end_addr) - static_cast<char*>(start_addr);

    std::memset(ptr, pattern, size);
    for (size_t i = 0; i < size; ++i) {
        if (ptr[i] != pattern) {
            std::cerr << "[ERRO] Corrupção de dados detectada no bloco " << start_block << "\n";
            return false;
        }
    }
    return true;
}

// -------------------------------------------------------------------
//  Ataques de Estresse
// -------------------------------------------------------------------
static void exploit_lifecycle_and_init() {
    std::cout << "\n>>> EXPLOTANDO 1: Falhas de Inicialização e Estado Antecipado\n";
    uint32_t dummy_block = 0;
    void* dummy_ptr1 = nullptr;
    void* dummy_ptr2 = nullptr;
    uint64_t dummy_tot_blocks = 0;
    uint64_t dummy_blk_size = 0;
    size_t dummy_use_size = 0;

    std::cout << " -> Tentando alocar antes de inicializar... Pronto.\n";
    memorymanager::memory_manager_alloc(999, 1, dummy_block);
    std::cout << " -> Tentando liberar antes de inicializar... Pronto.\n";
    memorymanager::memory_manager_free(999);
    std::cout << " -> Tentando obter endereço antes de inicializar... Pronto.\n";
    memorymanager::memory_manager_get_address(0, 1, dummy_ptr1, dummy_ptr2);
    std::cout << " -> Tentando obter info antes de inicializar... Pronto.\n";
    memorymanager::memory_manager_info(dummy_tot_blocks, dummy_blk_size, dummy_use_size);

    std::cout << " -> Tentando inicializar com tamanho 0... Pronto.\n";
    memorymanager::memory_manager_init(0);
    std::cout << " -> Tentando inicializar com tamanho máximo (Overflow)... Pronto.\n";
    memorymanager::memory_manager_init(UINT64_MAX);
}

static void exploit_loan_table_overflow(uint64_t total_blocks) {
    std::cout << "\n>>> EXPLOTANDO 2: Transbordo da Tabela Estática de Alocações (max_loans)\n";
    std::cout << " -> Bombardeando alocações de bloco único com IDs de threads incrementais...\n";

    uint32_t start_block = 0;
    uint32_t successful_loans = 0;

    for (uint64_t tid = 5000; tid < 5000 + (total_blocks + 2000); ++tid) {
        int ret = memorymanager::memory_manager_alloc(tid, 1, start_block);
        if (ret == memorymanager::MANAGER_OK) {
            successful_loans++;
            fill_and_verify(start_block, 1, 0xEE);
        } else if (ret == memorymanager::MANAGER_ERR_TABLE_FULL || ret == memorymanager::MANAGER_ERR_NO_SPACE) {
            std::cout << " -> Barreira alcançada de forma limpa pelo gerenciador em " << successful_loans << " alocações.\n";
            break;
        }
    }

    std::cout << " -> Executando varredura de liberação violenta...\n";
    for (uint64_t tid = 4900; tid < 5000 + successful_loans + 100; ++tid) {
        memorymanager::memory_manager_free(tid);
    }
}

static void exploit_thread_id_collisions() {
    std::cout << "\n>>> EXPLOTANDO 3: Colisões de Thread ID e Liberação Dupla (Double Free)\n";
    uint32_t b1 = 0, b2 = 0, b3 = 0;

    std::cout << " -> Thread 42 alocando bloco de tamanho 1...\n";
    memorymanager::memory_manager_alloc(42, 1, b1);
    std::cout << " -> Mesma Thread 42 alocando bloco de tamanho 5... ";
    int ret = memorymanager::memory_manager_alloc(42, 5, b2);

    if (ret == memorymanager::MANAGER_OK) {
        std::cout << "Permitido! Verificando integridade física dos blocos simultâneos...\n";
        fill_and_verify(b1, 1, 0x11);
        fill_and_verify(b2, 5, 0x22);
    } else {
        std::cout << "Negado pelo gerenciador.\n";
    }

    std::cout << " -> Efetuando liberação da Thread 42 (Primeira vez)...\n";
    memorymanager::memory_manager_free(42);
    std::cout << " -> Efetuando liberação da Thread 42 (Segunda vez / Double Free)...\n";
    memorymanager::memory_manager_free(42);

    std::cout << " -> Nova thread assumindo o controle para testar corrupção estrutural... ";
    memorymanager::memory_manager_alloc(43, 2, b3);
    std::cout << "Pronto.\n";
    memorymanager::memory_manager_free(43);
}

static void exploit_integer_overflows(uint64_t total_blocks) {
    std::cout << "\n>>> EXPLOTANDO 4: Estouro de Inteiros na Aritmética de Blocos\n";
    void* s_addr = nullptr; void* e_addr = nullptr;

    std::cout << " -> Solicitando endereço com num_blocks = UINT32_MAX... \n";
    memorymanager::memory_manager_get_address(0, UINT32_MAX, s_addr, e_addr);

    std::cout << " -> Solicitando endereço fora do escopo físico (start_block = total_blocks + 5000)... \n";
    memorymanager::memory_manager_get_address(static_cast<uint32_t>(total_blocks + 5000), 1, s_addr, e_addr);

    std::cout << " -> Solicitando endereço combinando deslocamento limite e estouro... \n";
    memorymanager::memory_manager_get_address(static_cast<uint32_t>(total_blocks - 1), 10, s_addr, e_addr);
}

static void exploit_extreme_fragmentation(uint64_t total_blocks) {
    std::cout << "\n>>> EXPLOTANDO 5: Fragmentação de Tabuleiro de Xadrez e Esmagamento First-Fit\n";

    // Capa o xadrez em 500 ou no limite total de blocos
    uint32_t target_blocks = (total_blocks > 500) ? 500 : static_cast<uint32_t>(total_blocks);
    std::vector<Allocation> active_allocs;

    std::cout << " -> Realizando alocações sequenciais alternadas de 1 bloco...\n";
    for (uint32_t i = 0; i < target_blocks; ++i) {
        uint32_t sb = 0;
        uint64_t tid = 10000 + i;
        if (memorymanager::memory_manager_alloc(tid, 1, sb) == memorymanager::MANAGER_OK) {
            active_allocs.push_back({tid, sb, 1});
            fill_and_verify(sb, 1, 0xAA);
        }
    }

    std::cout << " -> Liberando apenas índices pares para criar lacunas isoladas de tamanho 1...\n";
    for (size_t i = 0; i < active_allocs.size(); i += 2) {
        memorymanager::memory_manager_free(active_allocs[i].thread_id);
    }

    // --- ENCURRALANDO O ALOCADOR ---
    // Preenchemos toda a área limpa residual que sobrou após os 500 blocos
    uint32_t clean_zone_start = 0;
    uint32_t remaining_blocks = static_cast<uint32_t>(total_blocks) - target_blocks;
    bool locked_clean_zone = false;

    if (remaining_blocks > 0) {
        std::cout << " -> Isolando o teste: Trancando os " << remaining_blocks << " blocos limpos residuais...\n";
        if (memorymanager::memory_manager_alloc(99998, remaining_blocks, clean_zone_start) == memorymanager::MANAGER_OK) {
            locked_clean_zone = true;
        }
    }

    std::cout << " -> Forçando alocação impossível de tamanho 2 em espaço totalmente fragmentado... ";
    uint32_t fail_block = 0;
    int ret = memorymanager::memory_manager_alloc(99999, 2, fail_block);

    if (ret == memorymanager::MANAGER_OK) {
        // Se caiu aqui dentro da zona do xadrez (< target_blocks), é erro crônico de First-Fit!
        if (fail_block < target_blocks) {
            std::cout << "\n[ALERTA CRÍTICO] Bug detectado! O Gerenciador quebrou o First-Fit e alocou contíguo no bloco " << fail_block << "!\n";
            fill_and_verify(fail_block, 2, 0x66);
            memorymanager::memory_manager_free(99999);
        } else {
            std::cout << "\n[INFO] Alocou na zona residual (bloco " << fail_block << "). Permitido por haver espaço limpa lá.\n";
            memorymanager::memory_manager_free(99999);
        }
    } else {
        std::cout << "Recusado corretamente (Sem espaço contíguo real disponível!).\n";
    }

    // Desfaz os bloqueios estruturais
    if (locked_clean_zone) {
        memorymanager::memory_manager_free(99998);
    }
    for (size_t i = 1; i < active_allocs.size(); i += 2) {
        memorymanager::memory_manager_free(active_allocs[i].thread_id);
    }
}

static void exploit_post_shutdown() {
    std::cout << "\n>>> EXPLOTANDO 6: Chamadas Operacionais Pós-Destruição da Região\n";
    uint32_t dummy_block = 0;

    std::cout << " -> Desligando o gerenciador de memória (Shutdown)...\n";
    memorymanager::memory_manager_shutdown();

    std::cout << " -> Tentando alocar bloco pós-shutdown... Pronto.\n";
    memorymanager::memory_manager_alloc(888, 1, dummy_block);
    std::cout << " -> Tentando liberar thread pós-shutdown... Pronto.\n";
    memorymanager::memory_manager_free(888);
}

// -------------------------------------------------------------------
//  Main
// -------------------------------------------------------------------
int main(int argc, char* argv[]) {
    uint64_t requested_gb = DEFAULT_GB;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            return 0;
        }
        parse_memory_arg(argv[i], requested_gb);
    }

    size_t total_bytes = requested_gb * 1024ULL * 1024ULL * 1024ULL;

    std::cout << "==================================================================\n";
    std::cout << "       EXECUTANDO SUÍTE DE ESTRESSE DESTRUTIVA DO MEMORY MANAGER  \n";
    std::cout << "==================================================================\n";

    exploit_lifecycle_and_init();

    std::cout << "\n -> Inicializando Gerenciador para testes internos com " << requested_gb << " Gb...\n";
    int ret = memorymanager::memory_manager_init(total_bytes);
    if (ret != memorymanager::MANAGER_OK) {
        std::cerr << "Falha crítica: Não foi possível inicializar para os testes. Código: " << ret << "\n";
        return 1;
    }

    uint64_t total_blocks = 0, block_size = 0;
    size_t usable_size = 0;
    memorymanager::memory_manager_info(total_blocks, block_size, usable_size);

    exploit_loan_table_overflow(total_blocks);
    exploit_thread_id_collisions();
    exploit_integer_overflows(total_blocks);
    exploit_extreme_fragmentation(total_blocks);
    exploit_post_shutdown();

    std::cout << "\n==================================================================\n";
    std::cout << " FIM DOS TESTES: Se o programa não sofreu crash (SegFault), o seu\n";
    std::cout << " gerenciador passou com sucesso nas validações de segurança básicas.\n";
    std::cout << "==================================================================\n";

    return 0;
}
