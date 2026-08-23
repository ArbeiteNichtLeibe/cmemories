// request_queue.cpp
#include "../include/request_queue.hpp"
#include <chrono>
#include <cstring>

namespace memorymanager {

// ============================================================
// CONSTRUTOR
// ============================================================

RequestQueue::RequestQueue() {
    for (auto& req : m_request_queue) {
        req.type = RequestType::NONE;
        req.completed = true;
    }
    for (auto& entry : m_responses) {
        entry.valid = false;
        entry.request_id = 0u;
    }
    m_req_head = 0u;
    m_req_tail = 0u;
    m_req_count = 0u;
    m_res_head = 0u;
    m_res_tail = 0u;
    m_res_count = 0u;
}

// ============================================================
// CLIENTE - Envia requisição (non-blocking)
// ============================================================

uint64_t RequestQueue::send_request(const MemoryRequest& request) {
    if (m_shutdown.load() || !m_running.load()) {
        return 0u;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_req_count >= MAX_QUEUE) {
        return 0u;
    }
    
    MemoryRequest req = request;
    req.request_id = m_next_request_id.fetch_add(1);
    req.completed = false;
    
    m_request_queue[m_req_tail] = req;
    m_req_tail = (m_req_tail + 1u) % MAX_QUEUE;
    ++m_req_count;
    
    m_cond.notify_one();
    return req.request_id;
}

bool RequestQueue::try_get_response(uint64_t request_id, MemoryRequest& response) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    for (uint32_t i = 0u; i < MAX_PENDING; ++i) {
        uint32_t idx = (m_res_head + i) % MAX_PENDING;
        if (m_responses[idx].valid && m_responses[idx].request_id == request_id) {
            response = m_responses[idx].response;
            m_responses[idx].valid = false;
            --m_res_count;
            // Avança a cabeça se possível
            while (m_res_count > 0u && !m_responses[m_res_head].valid) {
                m_res_head = (m_res_head + 1u) % MAX_PENDING;
            }
            return true;
        }
    }
    return false;
}

bool RequestQueue::wait_response(uint64_t request_id, MemoryRequest& response, 
                                 uint32_t timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::milliseconds(timeout_ms);
    
    while (m_running.load() && !m_shutdown.load()) {
        if (try_get_response(request_id, response)) {
            return true;
        }
        
        if (timeout_ms > 0u) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= timeout) {
                return false;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// ============================================================
// THREAD - Recebe requisição e envia resposta
// ============================================================

bool RequestQueue::receive_request(MemoryRequest& request, uint32_t timeout_ms) {
    if (m_shutdown.load()) {
        return false;
    }
    
    std::unique_lock<std::mutex> lock(m_mutex);
    
    if (timeout_ms == 0u) {
        while (m_req_count == 0u && m_running.load()) {
            m_cond.wait(lock);
        }
    } else {
        auto deadline = std::chrono::steady_clock::now() + 
                        std::chrono::milliseconds(timeout_ms);
        while (m_req_count == 0u && m_running.load()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            m_cond.wait_until(lock, deadline);
        }
    }
    
    if (!m_running.load() || m_shutdown.load() || m_req_count == 0u) {
        return false;
    }
    
    request = m_request_queue[m_req_head];
    m_req_head = (m_req_head + 1u) % MAX_QUEUE;
    --m_req_count;
    
    m_cond.notify_one();
    return true;
}

bool RequestQueue::send_response(const MemoryRequest& response) {
    if (m_shutdown.load()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_res_count >= MAX_PENDING) {
        return false;
    }
    
    // Procura slot vazio a partir da cauda
    uint32_t slot = m_res_tail;
    uint32_t start = m_res_tail;
    do {
        if (!m_responses[slot].valid) {
            break;
        }
        slot = (slot + 1u) % MAX_PENDING;
    } while (slot != start);
    
    if (m_responses[slot].valid) {
        return false; // Não encontrou slot vazio
    }
    
    m_responses[slot].request_id = response.request_id;
    m_responses[slot].response = response;
    m_responses[slot].valid = true;
    ++m_res_count;
    m_res_tail = (m_res_tail + 1u) % MAX_PENDING;
    
    m_cond.notify_one();
    return true;
}

// ============================================================
// CONTROLE
// ============================================================

bool RequestQueue::empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_req_count == 0u && m_res_count == 0u;
}

uint32_t RequestQueue::pending_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_res_count;
}

uint32_t RequestQueue::request_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_req_count;
}

void RequestQueue::shutdown() {
    m_shutdown.store(true);
    m_running.store(false);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cond.notify_all();
}

} // namespace memorymanager