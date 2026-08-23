// memorymanager.hpp
#ifndef MEMORYMANAGER_HPP
#define MEMORYMANAGER_HPP

#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <array>

namespace memorymanager {

enum class RequestType : uint32_t {
    ALLOC = 1u,
    FREE = 2u,
    GET_ADDRESS = 3u,
    GET_INFO = 4u,
    GET_ALLOCATED = 5u,
    SHUTDOWN = 6u
};

struct MemoryRequest {
    RequestType type{RequestType::ALLOC};
    uint64_t thread_id{0u};
    uint32_t num_blocks{0u};
    uint32_t start_block{0u};
    void* start_addr{nullptr};
    void* end_addr{nullptr};
    uint64_t total_blocks{0u};
    uint64_t block_size{0u};
    size_t usable_size{0u};
    uint64_t allocated_blocks{0u};
    int result{0};
    bool completed{false};
};

class MemoryManager {
public:
    MemoryManager() = default;
    ~MemoryManager();

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    bool init(size_t total_size);
    bool send_request(MemoryRequest& request);
    void shutdown();

    // Interface compatível (retorno bool)
    bool memory_manager_alloc(uint64_t thread_id, uint32_t num_blocks,
                              uint32_t& start_block_index);
    bool memory_manager_free(uint64_t thread_id);
    bool memory_manager_get_address(uint32_t start_block, uint32_t num_blocks,
                                    void*& start_addr, void*& end_addr);
    bool memory_manager_info(uint64_t& total_blocks, uint64_t& block_size,
                             size_t& usable_size);
    bool memory_manager_allocated_blocks(uint64_t& allocated_blocks);

private:
    static void* thread_func(void* arg);
    void process_loop();
    void process_request(MemoryRequest& request);

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
    
    static constexpr uint32_t MAX_QUEUE = 64u;
    std::array<MemoryRequest, MAX_QUEUE> m_queue{};
    uint32_t m_queue_head{0u};
    uint32_t m_queue_tail{0u};
    uint32_t m_queue_count{0u};
};

// Helpers
bool create_alloc_request(uint64_t thread_id, uint32_t num_blocks, 
                          MemoryRequest& request);
bool create_free_request(uint64_t thread_id, MemoryRequest& request);
bool create_address_request(uint32_t start_block, uint32_t num_blocks,
                            MemoryRequest& request);
bool create_info_request(MemoryRequest& request);
bool create_allocated_request(MemoryRequest& request);

} // namespace memorymanager

#endif // MEMORYMANAGER_HPP