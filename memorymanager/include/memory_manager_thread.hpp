// memory_manager_thread.hpp
#ifndef MEMORY_MANAGER_THREAD_HPP
#define MEMORY_MANAGER_THREAD_HPP

#include "request_queue.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace memorymanager {

// Forward declarations
struct BlockAllocator;

/**
 * @brief Thread principal do gerenciador de memória.
 * 
 * Utiliza RequestQueue para comunicação assíncrona com clientes.
 * Processa requisições em fila e retorna respostas.
 */
class MemoryManagerThread {
public:
    MemoryManagerThread();
    ~MemoryManagerThread();

    // ============================================================
    // CICLO DE VIDA
    // ============================================================
    
    bool init(uint32_t gigabytes);
    void shutdown();

    // ============================================================
    // INTERFACE PÚBLICA (Síncrona - Bloqueia até resposta)
    // ============================================================
    
    bool allocate(uint64_t thread_id, uint32_t megabytes,
                  uint32_t& start_block,
                  void*& start_address,
                  void*& end_address,
                  char* error_message = nullptr);

    bool free(uint64_t thread_id, char* error_message = nullptr);

    bool get_info(uint32_t& total_blocks,
                  size_t& block_size,
                  uint32_t& active_loans,
                  uint64_t& allocated_bytes);

    // ============================================================
    // ESTADO
    // ============================================================
    
    bool is_running() const { return m_running.load(); }
    bool is_initialized() const { return m_initialized.load(); }

private:
    // ============================================================
    // LOOP DA THREAD
    // ============================================================
    
    void thread_loop();
    void process_request(MemoryRequest& request);
    
    // ============================================================
    // PROCESSADORES DE REQUISIÇÃO
    // ============================================================
    
    void process_allocate(MemoryRequest& request);
    void process_free(MemoryRequest& request);
    void process_info(MemoryRequest& request);
    void process_stats(MemoryRequest& request);
    
    // ============================================================
    // UTILITÁRIOS
    // ============================================================
    
    uint32_t megabytes_to_blocks(uint32_t megabytes) const;
    void send_response(MemoryRequest& request);

    // ============================================================
    // MEMBROS
    // ============================================================
    
    // Thread e controle
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
    
    // Fila de requisições
    RequestQueue m_queue;
    
    // Mutex para respostas
    mutable std::mutex m_response_mutex;
    std::condition_variable m_response_cond;
    
    // Alocador e região
    BlockAllocator* m_allocator{nullptr};
    void* m_region_base{nullptr};
    size_t m_region_size{0u};
    
    // Estatísticas
    std::atomic<uint64_t> m_total_allocated_bytes{0u};
    std::atomic<uint32_t> m_total_allocations{0u};
    std::atomic<uint32_t> m_total_frees{0u};
};

} // namespace memorymanager

#endif // MEMORY_MANAGER_THREAD_HPP