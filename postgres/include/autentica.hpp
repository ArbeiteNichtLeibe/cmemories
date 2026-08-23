// autentica.hpp
#pragma once

#include <cstddef>
#include <cstdint>

class Authenticator {
public:
    // configPath: caminho para o arquivo de configuração (ex: "/etc/myapp/auth.secret")
    explicit Authenticator(const char* configPath);

    // Realiza o handshake sobre o socket já conectado.
    // Retorna true se autenticado, false em caso de falha (preenche errorMsg).
    bool authenticate(int sockFd, char* errorMsg, size_t errSize);

    // Verifica se a conexão ainda é válida (testa o estado do socket).
    // Retorna true se ativa, false se deve ser encerrada.
    bool isConnectionValid(int sockFd, char* errorMsg, size_t errSize);

    // Copia a resposta (ex: "AUTH_OK") para o buffer fornecido pelo script chamador.
    void getResponse(char* outBuffer, size_t bufferSize) const;

private:
    char configSecret_[4096];   // segredo lido do arquivo
    char response_[256];        // resposta final
    bool loaded_;               // se o config foi carregado com sucesso
};