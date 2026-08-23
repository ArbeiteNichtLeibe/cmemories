#include "http_server.hpp"
#include "auth.hpp"
#include "cookie.hpp"
#include "router.hpp"
#include "../../lerconfig/include/config.hpp"
#include "../../uteis/include/uteis.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <cerrno>

namespace http {

HttpServer& HttpServer::getInstance() {
    static HttpServer instance;
    return instance;
}

HttpServer::HttpServer()
    : mm(nullptr), redis(nullptr), tpm_man(nullptr), m_base_token(0),
      running(false), server_fd(-1), http_port(9010),
      req_received(0), req_served(0), errors(0) {
    std::memset(http_ip, 0, sizeof(http_ip));
    std::memset(http_docs, 0, sizeof(http_docs));
}

HttpServer::~HttpServer() {
    shutdown();
}

bool HttpServer::initialize(memorymanager::MemoryManagerThread* memory_manager,
                            FakeRedis::FakeRedis* redis_instance,
                            tpm2::TPMManager* tpm,
                            uint32_t base_token,
                            char* out_error, size_t err_size) {
    if (running.load()) return true;

    if (!memory_manager || !redis_instance || !tpm) {
        Utils::safeCopyString(out_error, err_size, "Null pointers provided");
        return false;
    }

    mm = memory_manager;
    redis = redis_instance;
    tpm_man = tpm;
    m_base_token = base_token;

    // Ler configurações
    LerConfig::Config& cfg = LerConfig::Config::getInstance();
    
    // Diagnóstico de carregamento
    http_port = static_cast<uint16_t>(cfg.getInt("http_port", 9010));
    const char* ip_str = cfg.getString("http_ip", "127.0.0.1");
    const char* docs = cfg.getString("http_docs", nullptr);

    fprintf(stdout, "⚙️ HttpServer configuration loaded:\n");
    fprintf(stdout, "   - Port: %d\n", http_port);
    fprintf(stdout, "   - IP: %s\n", ip_str ? ip_str : "NULL (MISSING)");
    fprintf(stdout, "   - Docs: %s\n", docs ? docs : "NULL (MISSING)");

    // Validações (sem fallback)
    if (!ip_str) {
        snprintf(out_error, err_size, "Missing 'http_ip' in config");
        return false;
    }
    strncpy(http_ip, ip_str, sizeof(http_ip) - 1);
    http_ip[sizeof(http_ip) - 1] = '\0';

    if (!docs) {
        snprintf(out_error, err_size, "Missing 'http_docs' in config");
        return false;
    }
    strncpy(http_docs, docs, sizeof(http_docs) - 1);
    http_docs[sizeof(http_docs) - 1] = '\0';

    // Tenta criar o diretório (se não existir)
    struct stat st;
    if (stat(http_docs, &st) != 0) {
        if (mkdir(http_docs, 0755) != 0) {
            snprintf(out_error, err_size, "Failed to create doc dir: %s", http_docs);
            return false;
        }
    }

    // Carregar/gerar token de autenticação
    const char* token_path = "/home/memorandos/webserver.conf";
    if (!auth::load_or_generate_token(token_path, tpm_man, out_error, err_size)) {
        return false;
    }

    // Criar socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        Utils::safeCopyString(out_error, err_size, "Socket creation failed");
        return false;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(http_port);
    if (inet_pton(AF_INET, http_ip, &addr.sin_addr) <= 0) {
        snprintf(out_error, err_size, "Invalid IP: %s", http_ip);
        close(server_fd);
        return false;
    }
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        snprintf(out_error, err_size, "Bind failed on port %d", http_port);
        close(server_fd);
        return false;
    }
    if (listen(server_fd, 128) < 0) {
        Utils::safeCopyString(out_error, err_size, "Listen failed");
        close(server_fd);
        return false;
    }

    running.store(true);

    uint64_t accept_tid = (static_cast<uint64_t>(m_base_token) << 32) | 0xFFFFFFFFULL;
    accept_thread_handle = std::thread(&HttpServer::accept_thread, this, accept_tid);

    for (int i = 0; i < NUM_WORKERS; ++i) {
        uint64_t worker_tid = (static_cast<uint64_t>(m_base_token) << 32) | static_cast<uint32_t>(i + 1);
        workers[i] = std::thread(&HttpServer::worker_thread, this, i, worker_tid);
    }

    return true;
}

void HttpServer::shutdown() {
    if (!running.load()) return;
    running.store(false);
    queue_cv.notify_all();

    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }

    if (accept_thread_handle.joinable()) accept_thread_handle.join();
    for (int i = 0; i < NUM_WORKERS; ++i) {
        if (workers[i].joinable()) workers[i].join();
    }

    // Liberar buffers pendentes
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        while (!task_queue.empty()) {
            Task& t = task_queue.front();
            if (t.buffer) {
                char err[256] = {0};
                mm->free(t.allocation_thread_id, err);
            }
            task_queue.pop();
        }
    }

    running.store(false);
}

void HttpServer::accept_thread(uint64_t thread_id) {
    while (running.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval tv = {0, 100000}; // 100ms
        int ret = select(server_fd + 1, &fds, nullptr, nullptr, &tv);
        if (ret > 0 && FD_ISSET(server_fd, &fds)) {
            int client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) continue;

            // Aloca buffer para cabeçalhos (1 MB)
            uint32_t start_block = 0;
            void* start_addr = nullptr;
            void* end_addr = nullptr;
            char err_buf[256] = {0};
            if (!mm->allocate(thread_id, HEADER_BUFFER_MB, start_block, start_addr, end_addr, err_buf)) {
                const char* err_resp =
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Type: application/json\r\n"
                    "Connection: close\r\n\r\n"
                    "{\"erro\":\"Buffer allocation failed\"}";
                write(client_fd, err_resp, strlen(err_resp));
                close(client_fd);
                errors++;
                continue;
            }

            char* buffer = static_cast<char*>(start_addr);
            ssize_t bytes = read(client_fd, buffer, HEADER_BUFFER_MB * 1024 * 1024 - 1);
            if (bytes <= 0) {
                mm->free(thread_id, err_buf);
                close(client_fd);
                continue;
            }
            buffer[bytes] = '\0';

            // Escolhe um worker (round‑robin)
            static std::atomic<int> worker_idx{0};
            int idx = worker_idx.fetch_add(1) % NUM_WORKERS;
            (void)idx;

            Task task;
            task.client_fd = client_fd;
            task.buffer = buffer;
            task.buffer_size = HEADER_BUFFER_MB * 1024 * 1024;
            task.bytes_read = static_cast<size_t>(bytes);
            task.allocation_thread_id = thread_id;

            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                task_queue.push(task);
            }
            queue_cv.notify_one();
            req_received++;
        }
    }
}

void HttpServer::worker_thread(int worker_id, uint64_t thread_id) {
    (void)worker_id;
    (void)thread_id;
    while (running.load()) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [this]() {
                return !task_queue.empty() || !running.load();
            });
            if (!running.load() && task_queue.empty()) break;
            task = task_queue.front();
            task_queue.pop();
        }

        process_request(task.client_fd, task.buffer, task.bytes_read, task.buffer_size, thread_id);

        char err[256] = {0};
        mm->free(task.allocation_thread_id, err);
        close(task.client_fd);
    }
}

void HttpServer::process_request(int client_fd, char* buffer, size_t bytes_read,
                                 size_t buffer_size, uint64_t worker_thread_id) {
    (void)buffer_size;
    (void)worker_thread_id;

    // ---------- AUTENTICAÇÃO ----------
    const char* auth_prefix = "Authorization: ";
    char* auth_pos = strstr(buffer, auth_prefix);
    bool auth_ok = false;
    if (auth_pos) {
        char* token_start = auth_pos + strlen(auth_prefix);
        char* token_end = strchr(token_start, '\r');
        if (token_end && (size_t)(token_end - token_start) == 64) {
            bool valid = true;
            for (size_t i = 0; i < 64; ++i) {
                if (!isxdigit(token_start[i])) { valid = false; break; }
            }
            if (valid) {
                auth_ok = auth::verify_token(token_start, 64);
            }
        }
    }
    if (!auth_ok) {
        send_unauthorized(client_fd);
        errors++;
        return;
    }

    // ---------- COOKIES ----------
    const char* cookie_prefix = "Cookie: ";
    char* cookie_pos = strstr(buffer, cookie_prefix);
    cookie::CookiePair pairs[16];
    size_t cookie_count = 0;
    if (cookie_pos) {
        char* cookie_start = cookie_pos + strlen(cookie_prefix);
        char* cookie_end = strstr(cookie_start, "\r\n");
        if (cookie_end) {
            *cookie_end = '\0';
            cookie_count = cookie::parse_cookies(cookie_start, pairs, 16);
            *cookie_end = '\r';
        }
    }
    (void)cookie_count;

    // ---------- MÉTODO E PATH ----------
    char* line_end = strstr(buffer, "\r\n");
    if (!line_end) {
        const char* bad = "HTTP/1.1 400 Bad Request\r\n\r\n";
        write(client_fd, bad, strlen(bad));
        errors++;
        return;
    }
    *line_end = '\0';
    char* method = buffer;
    char* method_end = strchr(method, ' ');
    if (!method_end) { *line_end = '\r'; return; }
    *method_end = '\0';
    char* path = method_end + 1;
    char* path_end = strchr(path, ' ');
    if (!path_end) { *line_end = '\r'; return; }
    *path_end = '\0';

    // ---------- CORPO ----------
    const char* body = strstr(line_end + 2, "\r\n\r\n");
    size_t body_len = 0;
    if (body) {
        body += 4;
        body_len = bytes_read - (body - buffer);
    }

    // ---------- ROTEAMENTO ----------
    router::RequestInfo req_info;
    req_info.method = method;
    req_info.path = path;
    req_info.body = body ? body : "";
    req_info.body_len = body_len;
    req_info.client_ip = "";
    req_info.user_agent = "";
    req_info.authorization = auth_pos ? (auth_pos + strlen(auth_prefix)) : "";

    router::Response resp;
    char router_err[256] = {0};
    if (!router::route_request(&req_info, &resp, router_err, sizeof(router_err))) {
        char err_resp[512];
        snprintf(err_resp, sizeof(err_resp),
                 "HTTP/1.1 500 Internal Server Error\r\n"
                 "Content-Type: application/json\r\n"
                 "Connection: close\r\n\r\n"
                 "{\"erro\":\"Router error: %s\"}", router_err);
        write(client_fd, err_resp, strlen(err_resp));
        errors++;
        return;
    }

    // ---------- RESPOSTA ----------
    char header[1024];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n",
        resp.status_code,
        (resp.status_code == 200 ? "OK" :
         resp.status_code == 400 ? "Bad Request" :
         resp.status_code == 401 ? "Unauthorized" :
         resp.status_code == 404 ? "Not Found" : "Error"),
        resp.content_type ? resp.content_type : "text/plain",
        resp.body_len);

    for (size_t i = 0; i < resp.cookie_count && i < 4; ++i) {
        const auto& c = resp.cookies[i];
        if (c.name && c.value) {
            header_len += snprintf(header + header_len, sizeof(header) - header_len,
                "Set-Cookie: %s=%s; Path=%s%s%s\r\n",
                c.name, c.value, c.path ? c.path : "/",
                c.secure ? "; Secure" : "",
                c.http_only ? "; HttpOnly" : "");
        }
    }
    header_len += snprintf(header + header_len, sizeof(header) - header_len, "\r\n");

    write(client_fd, header, header_len);
    if (resp.body && resp.body_len > 0) {
        write(client_fd, resp.body, resp.body_len);
    }

    req_served++;
}

void HttpServer::send_unauthorized(int client_fd) {
    const char* response =
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Bearer\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n\r\n"
        "{\"erro\":\"Token de autenticacao invalido ou ausente\"}";
    write(client_fd, response, strlen(response));
}

HttpServer::Stats HttpServer::get_stats() const {
    return { req_received.load(), req_served.load(), errors.load() };
}

} // namespace http