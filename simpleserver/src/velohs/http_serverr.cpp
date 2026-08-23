#include "../include/http_server.hpp"
#include "../include/auth.hpp"
#include "../include/cookie.hpp"
#include "../../lerconfig/include/config.hpp"
#include "../../jsonhandler/include/json_generator.hpp"
#include "../../jsonhandler/include/json_worker_instance.hpp" // processRequest
// ... outros includes ...

namespace http {

// ... (construtores, getInstance, etc.) ...

bool HttpServer::inicializar(memorymanager::MemoryManagerThread* memoryManager,
                             FakeRedis::FakeRedis* redisInstance,
                             uint32_t token,
                             char* outError, size_t errSize) {
    if (m_inicializado) return true;

    // ... (configuração de mm, redis, etc.) ...

    // Ler configurações via LerConfig
    LerConfig::Config& cfg = LerConfig::Config::getInstance();
    m_httpPort = cfg.getInt("http_port", 9010);
    const char* ipStr = cfg.getString("http_ip", "127.0.0.1");
    // Convertendo IP (usamos inet_aton ou similar)
    // ...

    // Carregar ou gerar token de autenticação (sempre exigido)
    const char* tokenPath = "/etc/memorandos/webserver.conf";
    if (!loadOrGenerateAuthToken(tokenPath, outError, errSize)) {
        return false;
    }

    // Inicializa socket, bind, listen, e thread de escuta...
    // ... (código existente) ...

    m_inicializado = true;
    return true;
}

void HttpServer::processarRequisicaoInPlace(int clientFd, char* requestBuffer, size_t bytesRead) {
    std::string_view req(requestBuffer, bytesRead);

    // ---------- AUTENTICAÇÃO ----------
    // Procurar header "Authorization: Bearer <token>"
    const char* authPrefix = "Authorization: ";
    size_t authPos = req.find(authPrefix);
    std::string_view authHeader;
    if (authPos != std::string_view::npos) {
        size_t end = req.find("\r\n", authPos);
        if (end != std::string_view::npos) {
            authHeader = req.substr(authPos + strlen(authPrefix), end - authPos - strlen(authPrefix));
        }
    }

    // Verificar token (sempre obrigatório)
    if (authHeader.size() < 7 || authHeader.substr(0, 7) != "Bearer ") {
        sendUnauthorized(clientFd);
        return;
    }
    std::string_view token = authHeader.substr(7);
    if (token.size() != 64 || !verifyBearerToken(token.data(), token.size())) {
        sendUnauthorized(clientFd);
        return;
    }

    // ---------- COOKIES ----------
    // Extrair cookies (opcional, para uso futuro)
    const char* cookiePrefix = "Cookie: ";
    size_t cookiePos = req.find(cookiePrefix);
    if (cookiePos != std::string_view::npos) {
        size_t end = req.find("\r\n", cookiePos);
        if (end != std::string_view::npos) {
            std::string_view cookieHeader = req.substr(cookiePos + strlen(cookiePrefix),
                                                       end - cookiePos - strlen(cookiePrefix));
            // Podemos parsear e armazenar em buffers locais (ou ignorar)
            // Exemplo: usar arrays temporários na stack (pequenos)
            char keys[10][64] = {};
            char values[10][256] = {};
            char* keyPtrs[10];
            char* valPtrs[10];
            size_t keySizes[10] = {64};
            size_t valSizes[10] = {256};
            for (size_t i = 0; i < 10; ++i) {
                keyPtrs[i] = keys[i];
                valPtrs[i] = values[i];
            }
            // parseCookies(...) – ignoramos o retorno aqui
        }
    }

    // ---------- ROTEAMENTO (restante) ----------
    // ... (código original com rotas GET /health, /stats, POST /api/query, etc.) ...
}

void HttpServer::sendUnauthorized(int clientFd) {
    const char* response =
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Bearer\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"erro\":\"Token de autenticacao invalido ou ausente\"}";
    write(clientFd, response, strlen(response));
}

// ... (demais métodos) ...

} // namespace http