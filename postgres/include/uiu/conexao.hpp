// conexao.hpp
// ============================================================
// GERENCIADOR DE CONEXÕES POSTGRESQL (C++ PURO)
// ============================================================

#ifndef CONEXAO_HPP
#define CONEXAO_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

#define MAX_PATH_LEN 108

// ============================================================
// ESTRUTURA DE CONFIGURAÇÃO DA CONEXÃO
// ============================================================

struct ConexaoConfig {
    char pgSocketPath[MAX_PATH_LEN];
    int clientPort;
    int keepaliveSeconds;
    int reconnectDelayMs;
    
    ConexaoConfig() : clientPort(0), keepaliveSeconds(30), reconnectDelayMs(5000) {
        pgSocketPath[0] = '\0';
    }
};

// ============================================================
// CLASSE GERENCIADORA DE CONEXÃO
// ============================================================

class GerenciadorConexao {
public:
    GerenciadorConexao();
    ~GerenciadorConexao();
    
    int abrirConexao(const ConexaoConfig& config, char* outError, size_t errSize);
    bool fecharConexao(int connectionId, char* outError, size_t errSize);
    void fecharTodasConexoes();
    bool ping(int connectionId);
    int getConexoesAtivas() const;

private:
    void* m_service;
    bool m_initialized;
};

// ============================================================
// FUNÇÕES C-STYLE
// ============================================================

int conexao_abrir(const char* pgSocketPath, int clientPort, char* outError, size_t errSize);
int conexao_fechar(int connectionId, char* outError, size_t errSize);
void conexao_fechar_todas(void);
int conexao_ping(int connectionId);
int conexao_get_active_count(void);

#endif