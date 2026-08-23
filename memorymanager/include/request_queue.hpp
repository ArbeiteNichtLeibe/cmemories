// request_queue.hpp
#ifndef REQUEST_QUEUE_HPP
#define REQUEST_QUEUE_HPP

#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cstdint>

namespace memorymanager {

enum class RequestType : uint32_t {
    ALLOCATE = 1u,
    FREE = 2u,
    SHUTDOWN = 3u,
    GET_INFO = 4u,
    GET_STATS = 5u,
    NONE = 0u
};

struct MemoryRequest {
    RequestType type{RequestType::NONE};
    uint64_t thread_id{0u};
    uint32_t num_megabytes{0u};
    uint32_t num_blocks{0u};
    
    // Resposta
    bool success{false};
    uint32_t start_block{0u};
    void* start_address{nullptr};
    void* end_address{nullptr};
    uint32_t total_blocks{0u};
    uint32_t active_loans{0u};
    uint64_t allocated_bytes{0u};
    char error_message[256]{};
    
    // Controle
    uint64_t request_id{0u};
    bool completed{false};
};

// ============================================================
// FILA ASSÍNCRONA (Non-Blocking)
// ============================================================

class RequestQueue {
public:
    static constexpr uint32_t MAX_QUEUE = 64u;
    static constexpr uint32_t MAX_PENDING = 256u;
    
    RequestQueue();
    ~RequestQueue() = default;
    
    // ============================================================
    // CLIENTE - Envia requisição (non-blocking)
    // ============================================================
    
    uint64_t send_request(const MemoryRequest& request);
    bool try_get_response(uint64_t request_id, MemoryRequest& response);
    bool wait_response(uint64_t request_id, MemoryRequest& response, 
                       uint32_t timeout_ms = 0u);
    
    // ============================================================
    // THREAD - Recebe requisição e envia resposta
    // ============================================================
    
    bool receive_request(MemoryRequest& request, uint32_t timeout_ms = 0u);
    bool send_response(const MemoryRequest& response);
    
    // ============================================================
    // CONTROLE
    // ============================================================
    
    bool empty() const;
    void shutdown();
    uint32_t pending_count() const;
    uint32_t request_count() const;

private:
    // Fila de requisições (cliente → thread)
    std::array<MemoryRequest, MAX_QUEUE> m_request_queue{};
    uint32_t m_req_head{0u};
    uint32_t m_req_tail{0u};
    uint32_t m_req_count{0u};
    
    // Fila de respostas (thread → cliente)
    struct ResponseEntry {
        uint64_t request_id{0u};
        MemoryRequest response{};
        bool valid{false};
    };
    std::array<ResponseEntry, MAX_PENDING> m_responses{};
    uint32_t m_res_head{0u};  // ← ESTAVA FALTANDO!
    uint32_t m_res_tail{0u};  // ← ESTAVA FALTANDO!
    uint32_t m_res_count{0u};
    
    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
    std::atomic<bool> m_running{true};
    std::atomic<bool> m_shutdown{false};
    std::atomic<uint64_t> m_next_request_id{1u};
};

} // namespace memorymanager

#endif // REQUEST_QUEUE_HPP