#include "router.hpp"
#include "../../uteis/include/uteis.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace http {
namespace router {

static const char* extract_json_field(const char* json, size_t json_len,
                                      const char* field_name,
                                      char* out_value, size_t out_size) {
    (void)json_len; // não usado, mas mantido para compatibilidade
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", field_name);
    const char* pos = strstr(json, search);
    if (!pos) return nullptr;
    pos = strchr(pos, ':');
    if (!pos) return nullptr;
    ++pos;
    while (*pos == ' ' || *pos == '\t') ++pos;
    if (*pos != '"') return nullptr;
    ++pos;
    const char* end = strchr(pos, '"');
    if (!end) return nullptr;
    size_t len = end - pos;
    if (len >= out_size) len = out_size - 1;
    std::memcpy(out_value, pos, len);
    out_value[len] = '\0';
    return out_value;
}

bool route_request(const RequestInfo* req, Response* resp,
                   char* out_error, size_t err_size) {
    if (!req || !resp) {
        Utils::safeCopyString(out_error, err_size, "Null request or response");
        return false;
    }

    resp->status_code = 404;
    resp->content_type = "application/json";
    resp->body = "{\"erro\":\"Rota nao encontrada\"}";
    resp->body_len = strlen(resp->body);
    resp->cookie_count = 0;

    // GET /health
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/health") == 0) {
        resp->status_code = 200;
        resp->body = "{\"status\":\"ok\"}";
        resp->body_len = strlen(resp->body);
        return true;
    }

    // GET /stats
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/stats") == 0) {
        resp->status_code = 200;
        resp->body = "{\"server\":\"Memorandos\",\"workers\":2}";
        resp->body_len = strlen(resp->body);
        return true;
    }

    // POST /api/query
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/query") == 0) {
        if (!req->body || req->body_len == 0) {
            resp->status_code = 400;
            resp->body = "{\"erro\":\"Corpo vazio\"}";
            resp->body_len = strlen(resp->body);
            return true;
        }
        char usuario[128] = {0};
        char dados[2048] = {0};
        if (!extract_json_field(req->body, req->body_len, "usuario", usuario, sizeof(usuario)) ||
            !extract_json_field(req->body, req->body_len, "dados", dados, sizeof(dados))) {
            resp->status_code = 400;
            resp->body = "{\"erro\":\"Campos 'usuario' e 'dados' obrigatorios\"}";
            resp->body_len = strlen(resp->body);
            return true;
        }
        // Em produção, geraria ticket e enfileiraria
        char resp_body[256];
        snprintf(resp_body, sizeof(resp_body),
                 "{\"status\":\"processando\",\"ticket\":123456789}");
        resp->status_code = 200;
        resp->body = resp_body; // CUIDADO: buffer local, apenas exemplo.
        resp->body_len = strlen(resp_body);
        return true;
    }

    // GET /api/result/{ticket}
    if (strcmp(req->method, "GET") == 0 && strncmp(req->path, "/api/result/", 12) == 0) {
        const char* ticket_str = req->path + 12;
        if (*ticket_str == '\0') {
            resp->status_code = 400;
            resp->body = "{\"erro\":\"Ticket ID nao informado\"}";
            resp->body_len = strlen(resp->body);
            return true;
        }
        char resp_body[128];
        snprintf(resp_body, sizeof(resp_body),
                 "{\"status\":\"aguardando\",\"ticket\":\"%s\"}", ticket_str);
        resp->status_code = 200;
        resp->body = resp_body;
        resp->body_len = strlen(resp_body);
        return true;
    }

    return true; // rota não encontrada já está configurada como 404
}

} // namespace router
} // namespace http