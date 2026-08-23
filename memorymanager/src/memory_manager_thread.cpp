// memory_manager_thread.cpp
#include "../include/memory_manager_thread.hpp"
#include "../include/createmmapa.hpp"
#include "../include/block_allocator.hpp"
#include "../include/block_address_calculator.hpp"
#include "../../uteis/include/uteis.hpp"  // ADICIONADO: para safeCopyString
#include <cstring>
#include <iostream>
#include <chrono>

namespace memorymanager {

static constexpr size_t MB = 1024ULL * 1024ULL;
static constexpr size_t BLOCK_SIZE = MB;

// ============================================================
// FUNÇÃO AUXILIAR PARA CÓPIA SEGURA (REMOVIDA - USAR Utils::safeCopyString)
// ============================================================

// A função safe_copy_string foi removida, pois agora usamos Utils::safeCopyString

// ============================================================
// CONSTRUTOR / DESTRUTOR
// ============================================================

MemoryManagerThread::MemoryManagerThread() {
 //   std::cout << "🔧 MemoryManagerThread constructor" << std::endl;
}

MemoryManagerThread::~MemoryManagerThread() {
  // std::cout << "🔧 MemoryManagerThread destructor" << std::endl;
    shutdown();
}

// ============================================================
// INICIALIZAÇÃO
// ============================================================

bool MemoryManagerThread::init(uint32_t gigabytes) {
   // std::cout << "📌 MemoryManagerThread::init(" << gigabytes << "GB)" << std::endl;
    
    if (m_initialized.load()) {
     //   std::cout << "   ⚠️  Already initialized" << std::endl;
        return false;
    }

    if (gigabytes < 1u || gigabytes > 30u) {
     //   std::cout << "   ❌ Invalid size: " << gigabytes << " (must be 1-30)" << std::endl;
        return false;
    }

    const size_t requested_size = static_cast<size_t>(gigabytes) * 1024ULL * 1024ULL * 1024ULL;
   // std::cout << "   📦 Requested size: " << requested_size << " bytes" << std::endl;

    // ============================================================
    // 1. ALOCA REGIÃO MMAP
    // ============================================================
    
  //  std::cout << "   📌 Step 1: Allocating mmap region..." << std::endl;
    if (!allocate_region(requested_size, m_region_base, m_region_size)) {
     //   std::cout << "   ❌ allocate_region failed!" << std::endl;
        return false;
    }
 //   std::cout << "   ✅ mmap region allocated: " << m_region_base << std::endl;

    // ============================================================
    // 2. INICIALIZA BLOCK ALLOCATOR
    // ============================================================
    
 //   std::cout << "   📌 Step 2: Initializing BlockAllocator..." << std::endl;
   static BlockAllocator allocator;
    m_allocator = &allocator;

    uint64_t usable_start = 0u;
    size_t usable_size = 0u;

    if (!init_block_allocator(m_region_base, m_region_size, 
                              *m_allocator, usable_start, usable_size)) {
     //   std::cout << "   ❌ init_block_allocator failed!" << std::endl;
        free_region(m_region_base, m_region_size);
        m_region_base = nullptr;
        m_region_size = 0u;
        return false;
    }
    //std::cout << "   ✅ BlockAllocator initialized: " 
          //    << m_allocator->total_blocks << " blocks" << std::endl;

    // ============================================================
    // 3. INICIA THREAD
    // ============================================================
    
   // std::cout << "   📌 Step 3: Starting main thread..." << std::endl;
    m_running.store(true);
    m_initialized.store(true);
    m_total_allocated_bytes.store(0u);

    try {
        m_thread = std::thread(&MemoryManagerThread::thread_loop, this);
       // std::cout << "   ✅ Main thread started (ID: " << m_thread.get_id() << ")" << std::endl;
    } catch (const std::exception& e) {
    //    std::cout << "   ❌ Failed to start thread: " << e.what() << std::endl;
        m_running.store(false);
        m_initialized.store(false);
        free_region(m_region_base, m_region_size);
        m_region_base = nullptr;
        m_region_size = 0u;
        return false;
    }

 //   std::cout << "✅ MemoryManagerThread::init() completed!" << std::endl;
    return true;
}

// ============================================================
// SHUTDOWN
// ============================================================

void MemoryManagerThread::shutdown() {
 //   std::cout << "📌 MemoryManagerThread::shutdown()" << std::endl;
    
    if (!m_initialized.load()) {
    //    std::cout << "   ⚠️  Already shut down" << std::endl;
        return;
    }

    m_running.store(false);
    m_initialized.store(false);

    m_queue.shutdown();

    if (m_thread.joinable()) {
        std::cout << "   ⏳ Waiting for thread to finish..." << std::endl;
        auto start = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(5);
        
        while (m_thread.joinable() && 
               std::chrono::steady_clock::now() - start < timeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        if (m_thread.joinable()) {
       //     std::cout << "   ⚠️  Timeout, detaching thread" << std::endl;
            m_thread.detach();
        } else {
       //     std::cout << "   ✅ Thread joined" << std::endl;
        }
    }

    if (m_region_base != nullptr && m_region_size > 0u) {
    //    std::cout << "   📌 Freeing mmap region..." << std::endl;
        free_region(m_region_base, m_region_size);
        m_region_base = nullptr;
        m_region_size = 0u;
        std::cout << "   ✅ mmap region freed" << std::endl;
    }

    m_allocator = nullptr;
 //   std::cout << "✅ MemoryManagerThread::shutdown() completed" << std::endl;
}

// ============================================================
// FUNÇÕES PÚBLICAS - ALLOCATE (CORRIGIDA)
// ============================================================

bool MemoryManagerThread::allocate(uint64_t thread_id, uint32_t megabytes,
                                   uint32_t& start_block,
                                   void*& start_address,
                                   void*& end_address,
                                   char* error_message) {
    // ============================================================
    // VERIFICAÇÃO DE ESTADO
    // ============================================================
    
    if (!m_initialized.load() || !m_running.load()) {
        Utils::safeCopyString(error_message, 256, "Thread not initialized");  // MODIFICADO
        return false;
    }

    // ============================================================
    // CONSTRUÇÃO DA REQUISIÇÃO
    // ============================================================
    
    MemoryRequest req;
    req.type = RequestType::ALLOCATE;
    req.thread_id = thread_id;
    req.num_megabytes = megabytes;
    req.completed = false;

    // ============================================================
    // ENVIO DA REQUISIÇÃO
    // ============================================================
    
    uint64_t req_id = m_queue.send_request(req);
    if (req_id == 0u) {
        Utils::safeCopyString(error_message, 256, "Failed to send request");  // MODIFICADO
        return false;
    }

    // ============================================================
    // ESPERA PELA RESPOSTA
    // ============================================================
    
    MemoryRequest response;
    if (!m_queue.wait_response(req_id, response, 5000)) {
        Utils::safeCopyString(error_message, 256, "Request timeout");  // MODIFICADO
        return false;
    }

    // ============================================================
    // VERIFICAÇÃO DA RESPOSTA
    // ============================================================
    
    if (!response.success) {
        Utils::safeCopyString(error_message, 256, response.error_message);  // MODIFICADO
        return false;
    }

    // ============================================================
    // PREENCHIMENTO DOS RESULTADOS
    // ============================================================
    
    start_block = response.start_block;
    start_address = response.start_address;
    end_address = response.end_address;
    return true;
}

// ============================================================
// FUNÇÕES PÚBLICAS - FREE (CORRIGIDA)
// ============================================================

bool MemoryManagerThread::free(uint64_t thread_id, char* error_message) {
    // ============================================================
    // VERIFICAÇÃO DE ESTADO
    // ============================================================
    
    if (!m_initialized.load() || !m_running.load()) {
        Utils::safeCopyString(error_message, 256, "Thread not initialized");  // MODIFICADO
        return false;
    }

    // ============================================================
    // CONSTRUÇÃO DA REQUISIÇÃO
    // ============================================================
    
    MemoryRequest req;
    req.type = RequestType::FREE;
    req.thread_id = thread_id;
    req.completed = false;

    // ============================================================
    // ENVIO DA REQUISIÇÃO
    // ============================================================
    
    uint64_t req_id = m_queue.send_request(req);
    if (req_id == 0u) {
        Utils::safeCopyString(error_message, 256, "Failed to send request");  // MODIFICADO
        return false;
    }

    // ============================================================
    // ESPERA PELA RESPOSTA
    // ============================================================
    
    MemoryRequest response;
    if (!m_queue.wait_response(req_id, response, 5000)) {
        Utils::safeCopyString(error_message, 256, "Request timeout");  // MODIFICADO
        return false;
    }

    // ============================================================
    // VERIFICAÇÃO DA RESPOSTA
    // ============================================================
    
    if (!response.success) {
        Utils::safeCopyString(error_message, 256, response.error_message);  // MODIFICADO
        return false;
    }

    return true;
}

// ============================================================
// FUNÇÕES PÚBLICAS - GET INFO
// ============================================================

bool MemoryManagerThread::get_info(uint32_t& total_blocks,
                                   size_t& block_size,
                                   uint32_t& active_loans,
                                   uint64_t& allocated_bytes) {
    if (!m_initialized.load() || !m_running.load()) {
        return false;
    }

    MemoryRequest req;
    req.type = RequestType::GET_INFO;
    req.completed = false;

    uint64_t req_id = m_queue.send_request(req);
    if (req_id == 0u) {
        return false;
    }

    MemoryRequest response;
    if (!m_queue.wait_response(req_id, response, 5000)) {
        return false;
    }

    if (!response.success) {
        return false;
    }

    total_blocks = response.total_blocks;
    block_size = m_allocator ? m_allocator->block_size : 0u;
    active_loans = response.active_loans;
    allocated_bytes = response.allocated_bytes;
    return true;
}

// ============================================================
// LOOP DA THREAD
// ============================================================

void MemoryManagerThread::thread_loop() {
     //std::cout << "🧵 Thread loop started (ID: " << std::this_thread::get_id() << ")" << std::endl;
    
    while (m_running.load()) {
        MemoryRequest request{};
        
        if (!m_queue.receive_request(request)) {
            if (!m_running.load()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

      //  std::cout << "🧵 Processing request type: " << static_cast<int>(request.type) 
  //                << " (ID: " << request.request_id << ")" << std::endl;

        process_request(request);
        request.completed = true;
        
        m_queue.send_response(request);
    }
    
  //  std::cout << "🧵 Thread loop finished" << std::endl;
}

// ============================================================
// PROCESSAMENTO DE REQUISIÇÕES
// ============================================================

void MemoryManagerThread::process_request(MemoryRequest& request) {
    switch (request.type) {
        case RequestType::ALLOCATE:
            process_allocate(request);
            break;
        case RequestType::FREE:
            process_free(request);
            break;
        case RequestType::GET_INFO:
            process_info(request);
            break;
        case RequestType::GET_STATS:
            process_stats(request);
            break;
        default:
            request.success = false;
            Utils::safeCopyString(request.error_message, sizeof(request.error_message), "Unknown request type");  // MODIFICADO
            break;
    }
}

void MemoryManagerThread::process_allocate(MemoryRequest& request) {
    if (m_allocator == nullptr) {
        request.success = false;
        Utils::safeCopyString(request.error_message, sizeof(request.error_message), "Allocator not initialized");  // MODIFICADO
        return;
    }

    if (request.num_megabytes == 0u) {
        request.success = false;
        Utils::safeCopyString(request.error_message, sizeof(request.error_message), "Requested size must be at least 1MB");  // MODIFICADO
        return;
    }

    const uint32_t num_blocks = (request.num_megabytes > 0u) ? request.num_megabytes : 1u;
    const int64_t result = allocate_blocks(*m_allocator, request.thread_id, num_blocks);

    if (result == -1) {
        request.success = false;
        Utils::safeCopyString(request.error_message, sizeof(request.error_message), "No contiguous space available");  // MODIFICADO
        return;
    }

    if (result == -2) {
        request.success = false;
        Utils::safeCopyString(request.error_message, sizeof(request.error_message), "Thread already has an active loan");  // MODIFICADO
        return;
    }

    request.success = true;
    request.start_block = static_cast<uint32_t>(result);
    request.num_blocks = num_blocks;

    BlockAllocatorAddressInfo info;
    info.data_start = m_allocator->data_start;
    info.block_size = m_allocator->block_size;
    info.total_blocks = m_allocator->total_blocks;

    if (!get_block_range_address(info, request.start_block, num_blocks,
                                 request.start_address, request.end_address,
                                 request.error_message, sizeof(request.error_message))) {
        request.success = false;
        Utils::safeCopyString(request.error_message, sizeof(request.error_message), "Failed to calculate address");  // MODIFICADO
        return;
    }

    const uint64_t allocated_bytes = static_cast<uint64_t>(num_blocks) * BLOCK_SIZE;
    m_total_allocated_bytes.fetch_add(allocated_bytes);
}

void MemoryManagerThread::process_free(MemoryRequest& request) {
    if (m_allocator == nullptr) {
        request.success = false;
        Utils::safeCopyString(request.error_message, sizeof(request.error_message), "Allocator not initialized");  // MODIFICADO
        return;
    }

    bool has_loan = false;
    uint32_t num_blocks = 0u;
    
    for (uint32_t i = 0u; i < m_allocator->active_loans; ++i) {
        if (m_allocator->loan_table[i].id == request.thread_id) {
            has_loan = true;
            num_blocks = m_allocator->loan_table[i].num_blocks;
            break;
        }
    }

    if (!has_loan) {
        request.success = false;
        Utils::safeCopyString(request.error_message, sizeof(request.error_message), "Thread has no active loan");  // MODIFICADO
        return;
    }

    if (!free_blocks_by_id(*m_allocator, request.thread_id)) {
        request.success = false;
        Utils::safeCopyString(request.error_message, sizeof(request.error_message), "Failed to free blocks");  // MODIFICADO
        return;
    }

    const uint64_t freed_bytes = static_cast<uint64_t>(num_blocks) * BLOCK_SIZE;
    m_total_allocated_bytes.fetch_sub(freed_bytes);

    request.success = true;
}

void MemoryManagerThread::process_info(MemoryRequest& request) {
    if (m_allocator == nullptr) {
        request.success = false;
        Utils::safeCopyString(request.error_message, sizeof(request.error_message), "Allocator not initialized");  // MODIFICADO
        return;
    }

    request.success = true;
    request.total_blocks = m_allocator->total_blocks;
    request.active_loans = m_allocator->active_loans;
    request.allocated_bytes = m_total_allocated_bytes.load();
}

void MemoryManagerThread::process_stats(MemoryRequest& request) {
    if (m_allocator == nullptr) {
        request.success = false;
        Utils::safeCopyString(request.error_message, sizeof(request.error_message), "Allocator not initialized");  // MODIFICADO
        return;
    }

    request.success = true;
    request.total_blocks = m_allocator->total_blocks;
    request.active_loans = m_allocator->active_loans;
    request.allocated_bytes = m_total_allocated_bytes.load();
}

} // namespace memorymanager