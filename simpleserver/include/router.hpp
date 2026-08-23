#pragma once

#include <cstddef>

namespace http {
namespace router {

// Estrutura para passar informações da requisição.
struct RequestInfo {
    const char* method;          // "GET", "POST", etc.
    const char* path;            // caminho completo
    const char* body;            // corpo da requisição (pode ser nulo)
    size_t body_len;
    const char* client_ip;
    const char* user_agent;      // opcional
    const char* authorization;   // token Bearer (já verificado)
};

// Estrutura para construir a resposta.
struct Response {
    int status_code;             // 200, 404, etc.
    const char* content_type;
    const char* body;
    size_t body_len;
    // Cookies a serem enviados (até 4)
    struct CookieSet {
        const char* name;
        const char* value;
        const char* path;
        int max_age;
        bool secure;
        bool http_only;
    } cookies[4];
    size_t cookie_count;
};

// Função principal de roteamento.
// Recebe a requisição e preenche a resposta.
// Retorna true se a rota foi encontrada e processada, false em erro.
bool route_request(const RequestInfo* req, Response* resp,
                   char* out_error, size_t err_size);

} // namespace router
} // namespace http