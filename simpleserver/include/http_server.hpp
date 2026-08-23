#pragma once

#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include "../../memorymanager/include/memory_manager_thread.hpp"
#include "../../fakeredis/include/fake_redis.hpp"
#include "../../tpm2/include/tpm2_manager.hpp"

namespace http {

class HttpServer {
public:
    static HttpServer& getInstance();

    bool initialize(memorymanager::MemoryManagerThread* mm,
                    FakeRedis::FakeRedis* redis,
                    tpm2::TPMManager* tpm,
                    uint32_t base_token,
                    char* out_error, size_t err_size);

    void shutdown();

    struct Stats {
        uint64_t requests_received;
        uint64_t requests_served;
        uint64_t errors;
    };
    Stats get_stats() const;

private:
    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    static constexpr int NUM_WORKERS = 2;
    static constexpr uint32_t HEADER_BUFFER_MB = 1;   // 1 MB para cabeçalhos

    void worker_thread(int worker_id, uint64_t thread_id);
    void accept_thread(uint64_t thread_id);
    void process_request(int client_fd, char* buffer, size_t bytes_read,
                         size_t buffer_size, uint64_t worker_thread_id);
    void send_unauthorized(int client_fd);

    memorymanager::MemoryManagerThread* mm;
    FakeRedis::FakeRedis* redis;
    tpm2::TPMManager* tpm_man;
    uint32_t m_base_token;

    std::atomic<bool> running;
    int server_fd;
    uint16_t http_port;
    char http_ip[16];
    char http_docs[256];

    std::thread workers[NUM_WORKERS];
    std::thread accept_thread_handle;

    struct Task {
        int client_fd;
        char* buffer;
        size_t buffer_size;
        size_t bytes_read;
        uint64_t allocation_thread_id;   // ID da thread que alocou o buffer (accept_thread)
    };
    std::queue<Task> task_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    std::atomic<uint64_t> req_received;
    std::atomic<uint64_t> req_served;
    std::atomic<uint64_t> errors;
};

} // namespace http