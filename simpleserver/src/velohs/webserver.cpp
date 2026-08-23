// arquivo  na pasta /webserver/src/webserver.cpp 
#include "../include/webserver.hpp"
#include "../../lerconfig/include/config.hpp"
#include "../../memorymanager/include/memory_manager_v2.hpp"
#include "../../jsonhandler/include/json_worker.hpp"
#include "../../utils/include/jsoniskilled.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <iostream>
#include <regex>
#include <algorithm>
#include <chrono>
#include <limits>
#include <set>

namespace WebServer {

// ============================================================
// SINGLETON
// ============================================================

WebServer* WebServer::instance_ = nullptr;
std::mutex WebServer::instance_mutex_;

WebServer* WebServer::getInstance() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (instance_ == nullptr) {
        instance_ = new WebServer();
    }
    return instance_;
}

void WebServer::destroyInstance() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    delete instance_;
    instance_ = nullptr;
}

// ============================================================
// FUNÇÃO AUXILIAR PARA GERAR JSON VIA JSONWORKER
// ============================================================

static std::string gerarJsonViaWorker(const std::string& templateName, 
                                       const std::map<std::string, std::string>& dados) {
    auto* worker = JSONWorker::JsonWorker::getInstance();
    if (!worker) {
        std::cerr << "💥 CRASH: JSON Worker não disponível!" << std::endl;
        std::abort();
    }
    
    uint64_t id = worker->solicitarJson(templateName, dados);
    if (id == 0) {
        std::cerr << "💥 CRASH: Falha ao solicitar JSON para template: " << templateName << std::endl;
        std::abort();
    }
    
    char* json = worker->aguardarJson(id, 2000);
    if (!json) {
        std::cerr << "💥 CRASH: Timeout ao aguardar JSON para template: " << templateName << std::endl;
        std::abort();
    }
    
    std::string resultado(json);
    free(json);
    return resultado;
}

// ============================================================
// CONSTRUTOR/DESTRUTOR
// ============================================================

WebServer::WebServer() 
    : memory_manager_(nullptr)
    , server_fd_(-1)
    , is_running_(false)
    , active_connections_(0)
    , request_counter_(0)
    , total_requests_(0)
    , total_errors_(0) {
    memset(&server_addr_, 0, sizeof(server_addr_));
    carregarConfiguracao();
}

WebServer::~WebServer() {
    stop();
}

// ============================================================
// CARREGAR CONFIGURAÇÃO
// ============================================================

void WebServer::carregarConfiguracao() {
    auto& cfg = LerConfig::Config::getInstance();
    cfg.carregar("/etc/memorandos/config.conf");
    
    config_.webport = cfg.getInt("webport", 9010);
    config_.docs_dir = cfg.getString("docs_dir", "/var/lib/memorandos/docs");
    
    config_.cors.enabled = cfg.getBool("cors_enabled", true);
    config_.cors.allow_origin = cfg.getString("cors_allow_origin", "*");
    config_.cors.allow_methods = cfg.getString("cors_allow_methods", "GET, POST, PUT, DELETE, OPTIONS");
    config_.cors.allow_headers = cfg.getString("cors_allow_headers", "Content-Type, Authorization, X-Requested-With");
    config_.cors.allow_credentials = cfg.getBool("cors_allow_credentials", true);
    config_.cors.max_age = cfg.getInt("cors_max_age", 86400);
    config_.cors.expose_headers = cfg.getString("cors_expose_headers", "");
}

// ============================================================
// INICIALIZAÇÃO - CRASH SE TEMPLATES NÃO REGISTRAREM
// ============================================================

bool WebServer::initialize(MemoryManagerV2& memory_manager) {
    memory_manager_ = &memory_manager;
    
    if (!memory_manager_->estaSaudavel()) {
        std::cerr << "💥 CRASH: MemoryManager não está saudável!" << std::endl;
        std::abort();
    }
    
    // ============================================================
    // REGISTRAR TEMPLATES DO WEBSERVER - CRASH SE FALHAR
    // ============================================================
    auto* worker = JSONWorker::JsonWorker::getInstance();
    if (!worker) {
        std::cerr << "💥 CRASH: JSON Worker não disponível para registrar templates!" << std::endl;
        std::abort();
    }
    

// Registra web_erro
worker->registrarTemplate("web_erro", R"({"success":false,"error":{"codigo":{{codigo}},"mensagem":"[[mensagem]]","detalhes":"[[detalhes]]"}})");

// Registra web_resposta
worker->registrarTemplate("web_resposta", R"({"success":{{success}},"mensagem":"[[mensagem]]","timestamp":{{timestamp}}})");

// Registra web_status
worker->registrarTemplate("web_status", R"({"status":"[[status]]","versao":"[[versao]]","timestamp":{{timestamp}}})");

    // VERIFICA SE OS TEMPLATES FORAM REGISTRADOS
    auto stats = worker->getStats();
    if (stats.templatesRegistrados < 8) {  // 5 padrão + 3 do web = 8
        std::cerr << "💥 CRASH: Templates não registrados corretamente! Total: " 
                  << stats.templatesRegistrados << " (esperado 8)" << std::endl;
        std::abort();
    }
    
    std::cout << "   ✅ Templates do WebServer registrados: web_erro, web_resposta, web_status (total: " 
              << stats.templatesRegistrados << ")" << std::endl;
    
    std::cout << "✅ WebServer inicializado" << std::endl;
    std::cout << "   - Porta: " << config_.webport << std::endl;
    std::cout << "   - CORS: " << (config_.cors.enabled ? "Habilitado" : "Desabilitado") << std::endl;
    
    return true;
}

// ============================================================
// START/STOP
// ============================================================

bool WebServer::start() {
    if (is_running_) return false;
    
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "Erro ao criar socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "Erro ao configurar SO_REUSEADDR: " << strerror(errno) << std::endl;
        close(server_fd_);
        return false;
    }
    
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);
    
    server_addr_.sin_family = AF_INET;
    server_addr_.sin_addr.s_addr = INADDR_ANY;
    server_addr_.sin_port = htons(config_.webport);
    
    if (bind(server_fd_, (struct sockaddr*)&server_addr_, sizeof(server_addr_)) < 0) {
        std::cerr << "Erro ao fazer bind na porta " << config_.webport << ": " << strerror(errno) << std::endl;
        close(server_fd_);
        return false;
    }
    
    if (listen(server_fd_, SOMAXCONN) < 0) {
        std::cerr << "Erro ao ouvir conexões: " << strerror(errno) << std::endl;
        close(server_fd_);
        return false;
    }
    
    is_running_ = true;
    acceptor_thread_ = std::thread(&WebServer::acceptor_thread_func, this);
    
    for (int i = 0; i < 2; i++) {
        worker_threads_.emplace_back(&WebServer::worker_thread_func, this, i + 1);
    }
    
    std::cout << "\n🚀 WebServer rodando na porta " << config_.webport << std::endl;
    return true;
}

void WebServer::stop() {
    if (!is_running_) return;
    
    std::cout << "\n🛑 Parando WebServer..." << std::endl;
    is_running_ = false;
    
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    
    queue_cv_.notify_all();
    
    if (acceptor_thread_.joinable()) acceptor_thread_.join();
    
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) thread.join();
    }
    worker_threads_.clear();
    
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!ticket_queue_.empty()) {
            close_client_connection(ticket_queue_.front().client_fd);
            ticket_queue_.pop();
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(memory_blocks_mutex_);
        for (const auto& [thread_id, block_id] : thread_memory_blocks_) {
            if (memory_manager_ && block_id != 0) {
                memory_manager_->liberar(block_id);
            }
        }
        thread_memory_blocks_.clear();
    }
}

// ============================================================
// THREADS
// ============================================================

void WebServer::acceptor_thread_func() {
    std::cout << "📡 Acceptor thread iniciada" << std::endl;
    
    while (is_running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }
        
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        
        RequestTicket ticket;
        ticket.id = generate_ticket_id();
        ticket.client_fd = client_fd;
        ticket.timestamp = std::chrono::steady_clock::now();
        
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        ticket.client_ip = ip_str;
        ticket.client_port = ntohs(client_addr.sin_port);
        
        if (!queue_request(ticket)) {
            close_client_connection(client_fd);
        }
    }
    
    std::cout << "📡 Acceptor thread finalizada" << std::endl;
}

void WebServer::worker_thread_func(int thread_id) {
    std::cout << "👷 Worker thread " << thread_id << " iniciada" << std::endl;
    
    uint64_t memory_block_id = 0;
    if (memory_manager_) {
        memory_block_id = memory_manager_->solicitar(100 * 1024 * 1024);
        if (memory_block_id != 0) {
            std::cout << "   ✅ Thread " << thread_id << " alocou bloco " 
                      << memory_block_id << " (100MB)" << std::endl;
            
            std::lock_guard<std::mutex> lock(memory_blocks_mutex_);
            thread_memory_blocks_[thread_id] = memory_block_id;
        }
    }
    
    while (is_running_) {
        RequestTicket ticket = get_next_ticket();
        if (ticket.id == 0) continue;
        
        active_connections_++;
        
        if (parse_http_request(ticket.client_fd, ticket)) {
            Response response = process_request(ticket);
            send_response(ticket.client_fd, response);
            total_requests_++;
        } else {
            Response error = error_response("Bad Request", 400);
            send_response(ticket.client_fd, error);
            total_errors_++;
        }
        
        close_client_connection(ticket.client_fd);
        active_connections_--;
    }
    
    if (memory_manager_ && memory_block_id != 0) {
        memory_manager_->liberar(memory_block_id);
        std::cout << "   📤 Thread " << thread_id << " liberou bloco " 
                  << memory_block_id << std::endl;
        
        std::lock_guard<std::mutex> lock(memory_blocks_mutex_);
        thread_memory_blocks_.erase(thread_id);
    }
    
    std::cout << "👷 Worker thread " << thread_id << " finalizada" << std::endl;
}

// ============================================================
// PROCESSAMENTO DE REQUISIÇÕES
// ============================================================

Response WebServer::process_request(const RequestTicket& ticket) {
    std::cout << "🔄 Processando: " << ticket.method << " " << ticket.path << std::endl;
    
    if (ticket.method == "OPTIONS") {
        Response preflight_response;
        preflight_response.status_code = 204;
        preflight_response.status_text = "No Content";
        add_cors_headers(preflight_response);
        return preflight_response;
    }
    
    std::shared_lock<std::shared_mutex> lock(routes_mutex_);
    
    auto method_it = routes_.find(ticket.method);
    if (method_it != routes_.end()) {
        // Match exato
        auto path_it = method_it->second.find(ticket.path);
        if (path_it != method_it->second.end()) {
            try {
                Response response = path_it->second(ticket);
                add_cors_headers(response);
                return response;
            } catch (const std::exception& e) {
                std::cerr << "❌ Erro no handler: " << e.what() << std::endl;
                Response error = error_response(std::string("Internal Server Error: ") + e.what(), 500);
                add_cors_headers(error);
                return error;
            }
        }
        
        // Match com parâmetros
        for (const auto& [pattern, handler] : method_it->second) {
            if (pattern.find('{') != std::string::npos) {
                std::string regex_pattern = pattern;
                std::regex param_regex(R"(\{[^}]+\})");
                regex_pattern = std::regex_replace(regex_pattern, param_regex, "([^/]+)");
                regex_pattern = "^" + regex_pattern + "$";
                
                try {
                    std::regex route_regex(regex_pattern);
                    std::smatch match;
                    
                    if (std::regex_match(ticket.path, match, route_regex)) {
                        RequestTicket modified_ticket = ticket;
                        std::regex name_regex(R"(\{([^}]+)\})");
                        std::sregex_iterator name_it(pattern.begin(), pattern.end(), name_regex);
                        std::sregex_iterator name_end;
                        
                        int param_idx = 1;
                        for (; name_it != name_end; ++name_it, ++param_idx) {
                            std::string param_name = (*name_it)[1].str();
                            if (param_idx < (int)match.size()) {
                                modified_ticket.path_params[param_name] = match[param_idx].str();
                            }
                        }
                        
                        Response response = handler(modified_ticket);
                        add_cors_headers(response);
                        return response;
                    }
                } catch (const std::regex_error& e) {
                    continue;
                }
            }
        }
    }
    
    Response not_found = not_found_response();
    add_cors_headers(not_found);
    return not_found;
}

// ============================================================
// PARSE HTTP REQUEST
// ============================================================

bool WebServer::parse_http_request(int client_fd, RequestTicket& ticket) {
    std::vector<char> buffer(16384);
    std::string header_section;
    bool headers_complete = false;
    size_t content_length = 0;
    int read_attempts = 0;
    const int MAX_HEADER_READ_ATTEMPTS = 100;
    
    while (!headers_complete && is_running_ && read_attempts < MAX_HEADER_READ_ATTEMPTS) {
        ssize_t bytes_read = recv(client_fd, buffer.data(), buffer.size() - 1, 0);
        
        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                read_attempts++;
                continue;
            }
            return false;
        }
        
        if (bytes_read == 0) return false;
        
        buffer[bytes_read] = '\0';
        header_section.append(buffer.data(), bytes_read);
        
        if (header_section.size() > MAX_HEADER_SIZE) return false;
        
        size_t pos = header_section.find("\r\n\r\n");
        if (pos != std::string::npos) {
            headers_complete = true;
            
            std::string headers_part = header_section.substr(0, pos);
            std::string body_part = header_section.substr(pos + 4);
            
            size_t line_end = headers_part.find("\r\n");
            if (line_end == std::string::npos) return false;
            
            std::string request_line = headers_part.substr(0, line_end);
            std::istringstream iss(request_line);
            iss >> ticket.method >> ticket.path;
            
            if (ticket.method.empty()) return false;
            
            static const std::set<std::string> valid_methods = {
                "GET", "POST", "PUT", "DELETE", "OPTIONS", "HEAD", "PATCH"
            };
            
            if (valid_methods.find(ticket.method) == valid_methods.end()) return false;
            
            size_t query_pos = ticket.path.find('?');
            if (query_pos != std::string::npos) {
                ticket.query_string = ticket.path.substr(query_pos + 1);
                ticket.path = ticket.path.substr(0, query_pos);
                ticket.query_params = parse_query_string(ticket.query_string);
            }
            
            std::string remaining_headers = headers_part.substr(line_end + 2);
            ticket.headers = parse_headers(remaining_headers);
            
            auto content_length_it = ticket.headers.find("Content-Length");
            if (content_length_it != ticket.headers.end()) {
                try {
                    content_length = std::stoul(content_length_it->second);
                    if (content_length > MAX_BODY_SIZE) return false;
                } catch (...) {
                    return false;
                }
            }
            
            auto cookie_it = ticket.headers.find("Cookie");
            if (cookie_it != ticket.headers.end()) {
                if (cookie_it->second.size() <= MAX_COOKIE_SIZE) {
                    ticket.cookies = parse_cookies(cookie_it->second);
                }
            }
            
            ticket.body = body_part;
            break;
        }
        
        read_attempts++;
    }
    
    if (!headers_complete) return false;
    
    if (content_length > 0 && ticket.body.size() < content_length) {
        size_t remaining = content_length - ticket.body.size();
        ticket.body.reserve(content_length);
        
        while (remaining > 0 && is_running_) {
            size_t to_read = std::min(remaining, buffer.size());
            ssize_t bytes_read = recv(client_fd, buffer.data(), to_read, 0);
            
            if (bytes_read < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                return false;
            }
            
            if (bytes_read == 0) return false;
            
            ticket.body.append(buffer.data(), bytes_read);
            remaining -= bytes_read;
        }
        
        if (ticket.body.size() != content_length) return false;
    }
    
    return true;
}

// ============================================================
// RESPOSTAS JSON USANDO JSONWORKER - CRASH SE FALHAR
// ============================================================

Response WebServer::json_response(const std::string& templateName,
                                   const JsonData& dados,
                                   int status_code) {
    Response resp;
    resp.status_code = status_code;
    
    if (status_code == 200) resp.status_text = "OK";
    else if (status_code == 201) resp.status_text = "Created";
    else if (status_code == 204) resp.status_text = "No Content";
    else if (status_code == 400) resp.status_text = "Bad Request";
    else if (status_code == 401) resp.status_text = "Unauthorized";
    else if (status_code == 404) resp.status_text = "Not Found";
    else if (status_code == 405) resp.status_text = "Method Not Allowed";
    else if (status_code == 500) resp.status_text = "Internal Server Error";
    else resp.status_text = "Unknown";
    
    resp.body = gerarJsonViaWorker(templateName, dados);
    resp.headers["Content-Type"] = "application/json";
    return resp;
}

Response WebServer::error_response(const std::string& message, int status_code) {
    return json_response("web_erro", {
        {"codigo", std::to_string(status_code)},
        {"mensagem", message},
        {"detalhes", ""}
    }, status_code);
}

Response WebServer::success_response(const std::string& message) {
    return json_response("web_resposta", {
        {"success", "true"},
        {"mensagem", message},
        {"timestamp", std::to_string(std::time(nullptr))}
    }, 200);
}

Response WebServer::not_found_response() {
    return error_response("Rota não encontrada", 404);
}

Response WebServer::method_not_allowed_response() {
    return error_response("Método não permitido", 405);
}

// ============================================================
// JSON BODY PARSING
// ============================================================

nlohmann::json WebServer::get_json_body(const RequestTicket& ticket) {
    if (ticket.body.empty()) {
        throw std::runtime_error("Empty request body");
    }
    return RAGEmacao::parseJsonObjectSeguro(ticket.body, "WebServer::get_json_body");
}

// ============================================================
// REGISTRO DE ROTAS
// ============================================================

void WebServer::register_route(const std::string& method, const std::string& path, RequestHandler handler) {
    std::unique_lock<std::shared_mutex> lock(routes_mutex_);
    routes_[method][path] = handler;
    std::cout << "📝 Rota registrada: " << method << " " << path << std::endl;
}

void WebServer::register_json_route(const std::string& method, 
                                     const std::string& path,
                                     const std::string& templateName,
                                     JsonHandler handler) {
    register_route(method, path, [this, templateName, handler](const RequestTicket& ticket) {
        try {
            auto dados = handler(ticket);
            return json_response(templateName, dados, 200);
        } catch (const std::exception& e) {
            return error_response(e.what(), 500);
        }
    });
}

// ============================================================
// QUEUE
// ============================================================

bool WebServer::queue_request(const RequestTicket& ticket) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    
    if (!is_running_) return false;
    
    if (ticket.client_fd < 0) return false;
    
    if (ticket_queue_.size() >= MAX_QUEUE_SIZE) return false;
    
    ticket_queue_.push(ticket);
    queue_cv_.notify_one();
    return true;
}

RequestTicket WebServer::get_next_ticket() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    
    if (queue_cv_.wait_for(lock, std::chrono::milliseconds(100), 
        [this] { return !ticket_queue_.empty() || !is_running_; })) {
        
        if (!ticket_queue_.empty()) {
            RequestTicket ticket = ticket_queue_.front();
            ticket_queue_.pop();
            return ticket;
        }
    }
    
    return RequestTicket();
}

// ============================================================
// SEND RESPONSE
// ============================================================

void WebServer::send_response(int client_fd, const Response& response) {
    if (client_fd < 0) return;
    
    try {
        std::ostringstream oss;
        
        oss << "HTTP/1.1 " << response.status_code << " " << response.status_text << "\r\n";
        
        for (const auto& [key, value] : response.headers) {
            oss << key << ": " << value << "\r\n";
        }
        
        for (const auto& [name, value] : response.cookies) {
            oss << "Set-Cookie: " << name << "=" << value << "; Path=/; HttpOnly\r\n";
        }
        
        oss << "Content-Length: " << response.body.length() << "\r\n";
        oss << "Connection: close\r\n";
        oss << "\r\n";
        oss << response.body;
        
        std::string response_str = oss.str();
        send(client_fd, response_str.c_str(), response_str.length(), MSG_NOSIGNAL);
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Exception in send_response: " << e.what() << std::endl;
    }
}

// ============================================================
// CORS CONFIGURATION
// ============================================================

void WebServer::add_cors_headers(Response& response) {
    if (!config_.cors.enabled) return;
    
    response.headers["Access-Control-Allow-Origin"] = config_.cors.allow_origin;
    response.headers["Access-Control-Allow-Methods"] = config_.cors.allow_methods;
    response.headers["Access-Control-Allow-Headers"] = config_.cors.allow_headers;
    response.headers["Access-Control-Max-Age"] = std::to_string(config_.cors.max_age);
    
    if (config_.cors.allow_credentials) {
        response.headers["Access-Control-Allow-Credentials"] = "true";
    }
    
    if (!config_.cors.expose_headers.empty()) {
        response.headers["Access-Control-Expose-Headers"] = config_.cors.expose_headers;
    }
}

void WebServer::enable_cors(bool enable) {
    config_.cors.enabled = enable;
    std::cout << "🔧 CORS " << (enable ? "habilitado" : "desabilitado") << std::endl;
}

void WebServer::set_cors_origin(const std::string& origin) {
    config_.cors.allow_origin = origin;
    std::cout << "🔧 CORS Allow-Origin: " << origin << std::endl;
}

void WebServer::set_cors_methods(const std::string& methods) {
    config_.cors.allow_methods = methods;
    std::cout << "🔧 CORS Allow-Methods: " << methods << std::endl;
}

void WebServer::set_cors_headers(const std::string& headers) {
    config_.cors.allow_headers = headers;
    std::cout << "🔧 CORS Allow-Headers: " << headers << std::endl;
}

void WebServer::set_cors_credentials(bool allow) {
    config_.cors.allow_credentials = allow;
    std::cout << "🔧 CORS Allow-Credentials: " << (allow ? "true" : "false") << std::endl;
}

void WebServer::set_cors_max_age(int seconds) {
    config_.cors.max_age = seconds;
    std::cout << "🔧 CORS Max-Age: " << seconds << "s" << std::endl;
}

// ============================================================
// HELPERS
// ============================================================

void WebServer::close_client_connection(int client_fd) {
    if (client_fd >= 0) {
        close(client_fd);
    }
}

uint64_t WebServer::generate_ticket_id() {
    return ++request_counter_;
}

std::map<std::string, std::string> WebServer::parse_headers(const std::string& header_section) {
    std::map<std::string, std::string> headers;
    std::istringstream iss(header_section);
    std::string line;
    
    while (std::getline(iss, line) && !line.empty()) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t\r") + 1);
            
            if (key.size() < 64 && value.size() < 4096) {
                headers[key] = value;
            }
        }
    }
    
    return headers;
}

std::map<std::string, std::string> WebServer::parse_cookies(const std::string& cookie_header) {
    std::map<std::string, std::string> cookies;
    
    if (cookie_header.size() > MAX_COOKIE_SIZE) return cookies;
    
    std::istringstream iss(cookie_header);
    std::string token;
    
    while (std::getline(iss, token, ';')) {
        size_t eq = token.find('=');
        if (eq != std::string::npos) {
            std::string name = token.substr(0, eq);
            std::string value = token.substr(eq + 1);
            
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            
            if (!name.empty()) {
                cookies[name] = url_decode(value);
            }
        }
    }
    
    return cookies;
}

std::map<std::string, std::string> WebServer::parse_query_string(const std::string& query) {
    std::map<std::string, std::string> params;
    
    if (query.empty()) return params;
    
    std::istringstream iss(query);
    std::string token;
    
    while (std::getline(iss, token, '&')) {
        size_t eq = token.find('=');
        if (eq != std::string::npos) {
            std::string key = url_decode(token.substr(0, eq));
            std::string value = url_decode(token.substr(eq + 1));
            params[key] = value;
        } else {
            params[url_decode(token)] = "";
        }
    }
    
    return params;
}

std::string WebServer::url_decode(const std::string& encoded) {
    std::string decoded;
    decoded.reserve(encoded.length());
    
    for (size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            int value;
            std::istringstream iss(encoded.substr(i + 1, 2));
            if (iss >> std::hex >> value) {
                decoded += static_cast<char>(value);
                i += 2;
            } else {
                decoded += encoded[i];
            }
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }
    
    return decoded;
}

std::string WebServer::build_cookie_header(const std::map<std::string, std::string>& cookies) {
    std::string result;
    for (const auto& [name, value] : cookies) {
        if (!result.empty()) result += "; ";
        result += name + "=" + value;
    }
    return result;
}

} // namespace WebServer