#include "../include/auth_handler.hpp"
#include "../../sessoes/include/session_manager.hpp"
#include "../../jsonhandler/include/json_worker.hpp"
#include <iostream>
#include <map>
#include <ctime>

namespace AuthHandler {

static bool rotasRegistradas = false;
static bool templatesRegistrados = false;

static void registrarTemplatesAuth() {
    if (templatesRegistrados) return;
    
    auto* worker = JSONWorker::JsonWorker::getInstance();
    if (!worker) {
        std::cerr << "[AuthHandler] JSON Worker nao disponivel" << std::endl;
        return;
    }
    
    worker->registrarTemplate("auth_resposta", R"({"success":{{success}},"mensagem":"[[mensagem]]","timestamp":{{timestamp}}})");
    worker->registrarTemplate("auth_login", R"({"success":{{success}},"user_id":{{user_id}},"mensagem":"[[mensagem]]","session_key":"[[session_key]]"})");
    worker->registrarTemplate("auth_erro", R"({"success":false,"codigo":{{codigo}},"mensagem":"[[mensagem]]"})");
    
    templatesRegistrados = true;
    std::cout << "[AuthHandler] Templates registrados: auth_resposta, auth_login, auth_erro" << std::endl;
}

static std::string gerarJsonAuth(const std::string& templateName, 
                                  const std::map<std::string, std::string>& dados) {
    registrarTemplatesAuth();
    
    auto* worker = JSONWorker::JsonWorker::getInstance();
    if (!worker) {
        return "{\"success\":false,\"mensagem\":\"JSON Worker indisponivel\"}";
    }
    
    uint64_t id = worker->solicitarJson(templateName, dados);
    if (id == 0) {
        return "{\"success\":false,\"mensagem\":\"Falha ao solicitar JSON\"}";
    }
    
    char* json = worker->aguardarJson(id, 2000);
    if (!json) {
        return "{\"success\":false,\"mensagem\":\"Timeout ao gerar JSON\"}";
    }
    
    std::string resultado(json);
    free(json);
    return resultado;
}

WebServer::Response handleLogin(const WebServer::RequestTicket& ticket) {
    WebServer::Response response;
    response.headers["Content-Type"] = "application/json";
    
    int user_id = 2;
    
    if (!ticket.body.empty()) {
        try {
            std::string body_lower = ticket.body;
            size_t pos = body_lower.find("\"user_id\"");
            if (pos != std::string::npos) {
                size_t colon = body_lower.find(":", pos);
                if (colon != std::string::npos) {
                    size_t num_start = colon + 1;
                    while (num_start < body_lower.length() && 
                           (body_lower[num_start] == ' ' || body_lower[num_start] == '\t')) {
                        num_start++;
                    }
                    if (num_start < body_lower.length()) {
                        user_id = std::stoi(body_lower.substr(num_start));
                    }
                }
            }
        } catch (...) {}
    }
    
    auto& session_mgr = SessionManager::SessionManager::getInstance();
    int duracao = 86400;
    auto sessao = session_mgr.criarSessao(user_id, duracao);
    
    if (!sessao.valida) {
        response.status_code = 500;
        response.body = gerarJsonAuth("auth_erro", {
            {"codigo", "500"},
            {"mensagem", "Falha ao criar sessao: " + sessao.erro}
        });
        return response;
    }
    
    response.status_code = 200;
    response.body = gerarJsonAuth("auth_login", {
        {"success", "true"},
        {"user_id", std::to_string(user_id)},
        {"mensagem", "Login realizado com sucesso"},
        {"session_key", sessao.session_key}
    });
    response.cookies["session_id"] = sessao.session_key;
    
    return response;
}

WebServer::Response handleLogout(const WebServer::RequestTicket& ticket) {
    WebServer::Response response;
    response.headers["Content-Type"] = "application/json";
    
    std::string session_key;
    auto cookie_it = ticket.cookies.find("session_id");
    if (cookie_it != ticket.cookies.end()) {
        session_key = cookie_it->second;
    }
    
    if (!session_key.empty()) {
        auto& session_mgr = SessionManager::SessionManager::getInstance();
        session_mgr.destruirSessao(session_key);
    }
    
    response.status_code = 200;
    response.body = gerarJsonAuth("auth_resposta", {
        {"success", "true"},
        {"mensagem", "Logout realizado com sucesso"},
        {"timestamp", std::to_string(std::time(nullptr))}
    });
    response.cookies["session_id"] = "";
    
    return response;
}

WebServer::Response handleLoginSecreto(const WebServer::RequestTicket& ticket) {
    (void)ticket;
    
    WebServer::Response response;
    response.headers["Content-Type"] = "application/json";
    
    int user_id = 4;
    
    auto& session_mgr = SessionManager::SessionManager::getInstance();
    int duracao = 86400;
    auto sessao = session_mgr.criarSessao(user_id, duracao);
    
    if (!sessao.valida) {
        response.status_code = 500;
        response.body = gerarJsonAuth("auth_erro", {
            {"codigo", "500"},
            {"mensagem", "Falha ao criar sessao secreta: " + sessao.erro}
        });
        return response;
    }
    
    response.status_code = 200;
    response.body = gerarJsonAuth("auth_resposta", {
        {"success", "true"},
        {"mensagem", "Acesso liberado"},
        {"timestamp", std::to_string(std::time(nullptr))}
    });
    response.cookies["session_id"] = sessao.session_key;
    
    std::cout << "[AuthHandler] LoginSecreto: user_id=" << user_id << std::endl;
    
    return response;
}

void registrarRotasAuth(WebServer::WebServer* webserver) {
    if (rotasRegistradas) return;
    
    if (!webserver) {
        std::cerr << "[AuthHandler] WebServer nulo" << std::endl;
        return;
    }
    
    registrarTemplatesAuth();
    
    webserver->register_route("POST", "/login", handleLogin);
    webserver->register_route("POST", "/logout", handleLogout);
    webserver->register_route("GET", "/loginsecreto", handleLoginSecreto);
    webserver->register_route("POST", "/loginsecreto", handleLoginSecreto);
    
    rotasRegistradas = true;
    
    std::cout << "[AuthHandler] Rotas registradas: POST /login, POST /logout, GET/POST /loginsecreto" << std::endl;
}

} // namespace AuthHandler