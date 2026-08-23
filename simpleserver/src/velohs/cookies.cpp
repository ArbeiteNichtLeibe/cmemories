// Implementação de funções para gerenciamento de cookies
// ============================================================
// /webserver/src/include/cookies.cpp
#include "../../utils/include/jsoniskilled.hpp"
#include "../../webserver/include/webserver.hpp"
#include "../../jsonhandler/include/json_worker.hpp"
#include <chrono>
#include <iostream>
#include <random>

namespace Cookies {

// ============================================================
// FUNÇÕES AUXILIARES
// ============================================================

static uint32_t generateRandomSessionId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist(1, 0x7FFFFFFF);
    return dist(gen);
}

// ============================================================
// HANDLER PARA CRIAR COOKIE (USANDO JSONWORKER)
// ============================================================

WebServer::Response createCookies(const WebServer::RequestTicket& ticket) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    std::string session_id = "session_" + std::to_string(generateRandomSessionId()) + "_" + std::to_string(timestamp);
    std::string username = "teste";
    std::string hash = "hash_" + std::to_string(timestamp);

    // Usa JSONWorker para gerar a resposta
    WebServer::Response resp;
    resp.status_code = 200;
    resp.status_text = "OK";
    
    // Usa o template "resposta" do JSONWorker
    auto* worker = JSONWorker::JsonWorker::getInstance();
    if (worker) {
        std::map<std::string, std::string> dados = {
            {"success", "true"},
            {"mensagem", "Cookie criado para testes"},
            {"timestamp", std::to_string(timestamp)}
        };
        
        uint64_t id = worker->solicitarJson("resposta", dados);
        if (id != 0) {
            char* json = worker->aguardarJson(id, 2000);
            if (json) {
                resp.body = json;
                free(json);
            }
        }
    }
    
    if (resp.body.empty()) {
        // Fallback caso JSONWorker falhe
        resp.body = "{\"success\":true,\"mensagem\":\"Cookie criado para testes\"}";
    }
    
    resp.headers["Content-Type"] = "application/json";

    // Adiciona cookies na resposta
    resp.cookies["session_id"] = session_id;
    resp.cookies["username"] = username;
    resp.cookies["hash"] = hash;
    resp.cookies["login2000"] = "1";

    return resp;
}

// ============================================================
// VALIDAÇÃO DE COOKIE
// ============================================================

bool validarCookie(const WebServer::RequestTicket& ticket) {
    auto it = ticket.cookies.find("login2000");
    if (it == ticket.cookies.end()) {
        return false;
    }
    return !it->second.empty();
}

std::string criarCookieTeste() {
    return "session=abacate_" + std::to_string(time(nullptr)) + "; Path=/; HttpOnly";
}

// ============================================================
// ESTRUTURA PARA DADOS DA REQUISIÇÃO
// ============================================================

struct DadosRequisicao {
    std::string method;
    std::string path;
    std::map<std::string, std::string> cookies;
    std::string session_id;
    std::string username;
    std::string hash;
    std::string abacate;
    std::map<std::string, std::string> query_params;
    std::map<std::string, std::string> path_params;
    std::string content_type;
    std::string authorization;
    std::string origin;
    nlohmann::json body_json;
    bool body_valido = false;
    std::string body_erro;
};

DadosRequisicao extrairDadosRequisicao(const WebServer::RequestTicket& ticket) {
    DadosRequisicao dados;
    
    dados.method = ticket.method;
    dados.path = ticket.path;
    dados.cookies = ticket.cookies;
    
    auto it = ticket.cookies.find("session_id");
    if (it != ticket.cookies.end()) {
        dados.session_id = it->second;
    }
    
    it = ticket.cookies.find("username");
    if (it != ticket.cookies.end()) {
        dados.username = it->second;
    }
    
    it = ticket.cookies.find("hash");
    if (it != ticket.cookies.end()) {
        dados.hash = it->second;
    }
    
    it = ticket.cookies.find("abacate");
    if (it != ticket.cookies.end()) {
        dados.abacate = it->second;
    }
    
    dados.query_params = ticket.query_params;
    dados.path_params = ticket.path_params;
    
    auto header_it = ticket.headers.find("Content-Type");
    if (header_it != ticket.headers.end()) {
        dados.content_type = header_it->second;
    }
    
    header_it = ticket.headers.find("Authorization");
    if (header_it != ticket.headers.end()) {
        dados.authorization = header_it->second;
    }
    
    header_it = ticket.headers.find("Origin");
    if (header_it != ticket.headers.end()) {
        dados.origin = header_it->second;
    }
    
    // Usa parse seguro do jsoniskilled
    if (!ticket.body.empty()) {
        dados.body_json = RAGEmacao::parseJsonSeguro(ticket.body, "extrairDadosRequisicao");
        dados.body_valido = !dados.body_json.empty();
        if (!dados.body_valido) {
            dados.body_erro = "Falha no parse do JSON";
        }
    } else {
        dados.body_valido = false;
        dados.body_erro = "Body vazio";
    }
    
    return dados;
}

// ============================================================
// FUNÇÕES PARA EXTRAIR CAMPOS DO BODY
// ============================================================

template<typename T>
T getCampoJson(const DadosRequisicao& dados, const std::string& campo, const T& valor_padrao = T()) {
    if (!dados.body_valido) {
        return valor_padrao;
    }
    
    if (!dados.body_json.contains(campo)) {
        return valor_padrao;
    }
    
    if (dados.body_json[campo].is_null()) {
        return valor_padrao;
    }
    
    try {
        return dados.body_json[campo].get<T>();
    } catch (...) {
        return valor_padrao;
    }
}

inline std::string getPergunta(const DadosRequisicao& dados, const std::string& padrao = "") {
    return getCampoJson<std::string>(dados, "pergunta", padrao);
}

inline int getLimite(const DadosRequisicao& dados, int padrao = 5) {
    return getCampoJson<int>(dados, "limite", padrao);
}

inline std::string getTexto(const DadosRequisicao& dados, const std::string& padrao = "") {
    return getCampoJson<std::string>(dados, "texto", padrao);
}

inline std::string getNome(const DadosRequisicao& dados, const std::string& padrao = "") {
    return getCampoJson<std::string>(dados, "nome", padrao);
}

inline std::string getConteudo(const DadosRequisicao& dados, const std::string& padrao = "") {
    return getCampoJson<std::string>(dados, "conteudo", padrao);
}

// ============================================================
// ESTRUTURA COMPLETA PARA DADOS DA REQUISIÇÃO
// ============================================================

struct DadosCompletosRequisicao {
    std::string method;
    std::string path;
    std::string query_string;
    
    std::map<std::string, std::string> cookies;
    std::string session_id;
    std::string username;
    std::string hash;
    
    std::map<std::string, std::string> query_params;
    std::map<std::string, std::string> path_params;
    
    nlohmann::json body_json;
    bool body_valido = false;
    std::string body_erro;
    
    std::string content_type;
    std::string authorization;
    std::string origin;
    
    template<typename T>
    T getBodyField(const std::string& campo, const T& valor_padrao = T()) const {
        if (!body_valido || !body_json.contains(campo)) {
            return valor_padrao;
        }
        try {
            return body_json[campo].get<T>();
        } catch (...) {
            return valor_padrao;
        }
    }
    
    std::string getPergunta(const std::string& padrao = "") const {
        return getBodyField<std::string>("pergunta", padrao);
    }
    
    int getLimite(int padrao = 5) const {
        return getBodyField<int>("limite", padrao);
    }
    
    std::string getTexto(const std::string& padrao = "") const {
        return getBodyField<std::string>("texto", padrao);
    }
    
    std::string getNome(const std::string& padrao = "") const {
        return getBodyField<std::string>("nome", padrao);
    }
    
    std::string getConteudo(const std::string& padrao = "") const {
        return getBodyField<std::string>("conteudo", padrao);
    }
    
    std::string getQueryParam(const std::string& param, const std::string& padrao = "") const {
        auto it = query_params.find(param);
        return (it != query_params.end()) ? it->second : padrao;
    }
    
    std::string getPathParam(const std::string& param, const std::string& padrao = "") const {
        auto it = path_params.find(param);
        return (it != path_params.end()) ? it->second : padrao;
    }
};

// ============================================================
// FUNÇÃO PARA EXTRAIR DADOS COMPLETOS DA REQUISIÇÃO
// ============================================================

DadosCompletosRequisicao extrairDadosCompletos(const WebServer::RequestTicket& ticket) {
    DadosCompletosRequisicao dados;
    
    dados.method = ticket.method;
    dados.path = ticket.path;
    dados.query_string = ticket.query_string;
    
    dados.cookies = ticket.cookies;
    
    auto it = ticket.cookies.find("session_id");
    if (it != ticket.cookies.end()) {
        dados.session_id = it->second;
    }
    
    it = ticket.cookies.find("username");
    if (it != ticket.cookies.end()) {
        dados.username = it->second;
    }
    
    it = ticket.cookies.find("hash");
    if (it != ticket.cookies.end()) {
        dados.hash = it->second;
    }
    
    dados.query_params = ticket.query_params;
    dados.path_params = ticket.path_params;
    
    auto header_it = ticket.headers.find("Content-Type");
    if (header_it != ticket.headers.end()) {
        dados.content_type = header_it->second;
    }
    
    header_it = ticket.headers.find("Authorization");
    if (header_it != ticket.headers.end()) {
        dados.authorization = header_it->second;
    }
    
    header_it = ticket.headers.find("Origin");
    if (header_it != ticket.headers.end()) {
        dados.origin = header_it->second;
    }
    
    // Parse do body usando jsoniskilled
    if (!ticket.body.empty()) {
        dados.body_json = RAGEmacao::parseJsonSeguro(ticket.body, "extrairDadosCompletos");
        dados.body_valido = !dados.body_json.empty();
        
        if (dados.body_valido) {
            std::cout << "   ✅ Body JSON parseado com sucesso" << std::endl;
        } else {
            dados.body_erro = "Falha no parse do JSON";
            std::cout << "   ❌ Erro ao parsear body: " << dados.body_erro << std::endl;
        }
    } else {
        dados.body_valido = false;
        dados.body_erro = "Body vazio";
    }
    
    std::cout << "\n📋 DADOS DA REQUISIÇÃO:" << std::endl;
    std::cout << "   Method: " << dados.method << std::endl;
    std::cout << "   Path: " << dados.path << std::endl;
    std::cout << "   Query Params: " << dados.query_params.size() << std::endl;
    std::cout << "   Path Params: " << dados.path_params.size() << std::endl;
    std::cout << "   Cookies: " << dados.cookies.size() << std::endl;
    std::cout << "   Body válido: " << (dados.body_valido ? "Sim" : "Não") << std::endl;
    
    return dados;
}

// ============================================================
// FUNÇÕES SIMPLIFICADAS
// ============================================================

nlohmann::json getJsonBody(const WebServer::RequestTicket& ticket) {
    if (ticket.body.empty()) {
        throw std::runtime_error("Empty request body");
    }
    
    nlohmann::json result = RAGEmacao::parseJsonSeguro(ticket.body, "getJsonBody");
    if (result.empty()) {
        throw std::runtime_error("Invalid JSON format");
    }
    
    return result;
}

std::string getQueryParam(const WebServer::RequestTicket& ticket, 
                          const std::string& param, 
                          const std::string& padrao = "") {
    auto it = ticket.query_params.find(param);
    return (it != ticket.query_params.end()) ? it->second : padrao;
}

std::string getPathParam(const WebServer::RequestTicket& ticket, 
                         const std::string& param, 
                         const std::string& padrao = "") {
    auto it = ticket.path_params.find(param);
    return (it != ticket.path_params.end()) ? it->second : padrao;
}

template<typename T>
T getBodyField(const WebServer::RequestTicket& ticket, 
               const std::string& campo, 
               const T& valor_padrao = T()) {
    if (ticket.body.empty()) {
        return valor_padrao;
    }
    
    nlohmann::json body = RAGEmacao::parseJsonSeguro(ticket.body, "getBodyField");
    if (body.empty() || !body.contains(campo) || body[campo].is_null()) {
        return valor_padrao;
    }
    
    try {
        return body[campo].get<T>();
    } catch (...) {
        return valor_padrao;
    }
}

inline std::string getPerguntaBody(const WebServer::RequestTicket& ticket, 
                                    const std::string& padrao = "") {
    return getBodyField<std::string>(ticket, "pergunta", padrao);
}

inline int getLimiteBody(const WebServer::RequestTicket& ticket, int padrao = 5) {
    return getBodyField<int>(ticket, "limite", padrao);
}

} // namespace Cookies