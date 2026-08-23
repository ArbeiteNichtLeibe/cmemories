#include "worker.hpp"
#include <cstring>
#include <arpa/inet.h>
#include <chrono>

// ============================================================
// Ticket Implementation
// ============================================================

Ticket::Ticket(int ticketId)
    : id(ticketId), completed(false), failed(false) {}

// ============================================================
// TicketSystem Implementation
// ============================================================

TicketSystem::TicketSystem() : nextId(1) {}

std::shared_ptr<Ticket> TicketSystem::createTicket(const char* data, size_t len) {
    auto ticket = std::make_shared<Ticket>(nextId++);
    ticket->requestData.assign(data, data + len);
    
    {
        std::lock_guard<std::mutex> lock(ticketMutex);
        pendingTickets.push(ticket);
    }
    ticketCond.notify_one();
    
    return ticket;
}

std::shared_ptr<Ticket> TicketSystem::acquireTicket(int timeoutMs) {
    std::unique_lock<std::mutex> lock(ticketMutex);
    
    auto start = std::chrono::steady_clock::now();
    
    while (pendingTickets.empty()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        
        if (elapsed >= timeoutMs) {
            return nullptr;
        }
        
        ticketCond.wait_for(lock, std::chrono::milliseconds(100));
    }
    
    auto ticket = pendingTickets.front();
    pendingTickets.pop();
    return ticket;
}

void TicketSystem::returnTicket(std::shared_ptr<Ticket> ticket) {
    std::lock_guard<std::mutex> lock(ticketMutex);
    ticket->completed = true;
    completedTickets.push(ticket);
}

std::shared_ptr<Ticket> TicketSystem::collectResponse() {
    std::lock_guard<std::mutex> lock(ticketMutex);
    
    if (completedTickets.empty()) {
        return nullptr;
    }
    
    auto ticket = completedTickets.front();
    completedTickets.pop();
    return ticket;
}

size_t TicketSystem::pendingCount() const {
    std::lock_guard<std::mutex> lock(ticketMutex);
    return pendingTickets.size();
}

size_t TicketSystem::completedCount() const {
    std::lock_guard<std::mutex> lock(ticketMutex);
    return completedTickets.size();
}

// ============================================================
// Worker Implementation
// ============================================================

Worker::Worker(int id, int connId, 
               MemoryManagerV2* mm,
               ConnectionManager* mgr, 
               TicketSystem* ts,
               char* buffer,
               size_t bufSize)
    : workerId(id)
    , connectionId(connId)
    , memoryManager(mm)
    , manager(mgr)
    , ticketSystem(ts)
    , isRunning(true)
    , workBuffer(buffer)
    , bufferSize(bufSize)
    , pgBuffer(nullptr)
    , pgBufferSize(8192) {
    
    // Aloca buffer para pacotes PostgreSQL da ARENA
    if (memoryManager) {
        char errorBuffer[256];
        pgBuffer = static_cast<char*>(memoryManager->alocarSlot(pgBufferSize, 0, errorBuffer, sizeof(errorBuffer)));
    }
}

Worker::~Worker() {
    stop();
    
    // Libera buffer da arena
    if (memoryManager && pgBuffer) {
        char errorBuffer[256];
        memoryManager->liberarSlot(pgBuffer, errorBuffer, sizeof(errorBuffer));
        pgBuffer = nullptr;
    }
}

char* Worker::buildStartupPacket(const std::string& user, const std::string& database, size_t& outLen) {
    if (!pgBuffer) return nullptr;
    
    uint32_t totalSize = 4 + 4;
    totalSize += user.length() + 1;
    totalSize += database.length() + 1;
    totalSize += 1;
    
    if (totalSize > pgBufferSize) {
        return nullptr;
    }
    
    int pos = 0;
    
    uint32_t netSize = htonl(totalSize);
    memcpy(pgBuffer + pos, &netSize, 4);
    pos += 4;
    
    uint32_t protocolVersion = htonl(0x00030000);
    memcpy(pgBuffer + pos, &protocolVersion, 4);
    pos += 4;
    
    memcpy(pgBuffer + pos, "user", 4);
    pos += 4;
    memcpy(pgBuffer + pos, user.c_str(), user.length());
    pos += user.length();
    pgBuffer[pos++] = 0;
    
    memcpy(pgBuffer + pos, "database", 8);
    pos += 8;
    memcpy(pgBuffer + pos, database.c_str(), database.length());
    pos += database.length();
    pgBuffer[pos++] = 0;
    
    pgBuffer[pos++] = 0;
    
    outLen = totalSize;
    return pgBuffer;
}

char* Worker::buildQueryPacket(const std::string& query, size_t& outLen) {
    if (!pgBuffer) return nullptr;
    
    uint32_t msgSize = 4 + query.length() + 1;
    
    if (1 + msgSize > pgBufferSize) {
        return nullptr;
    }
    
    pgBuffer[0] = 'Q';
    
    uint32_t netSize = htonl(msgSize);
    memcpy(pgBuffer + 1, &netSize, 4);
    memcpy(pgBuffer + 5, query.c_str(), query.length());
    pgBuffer[5 + query.length()] = 0;
    
    outLen = 1 + msgSize;
    return pgBuffer;
}

void Worker::parseResponse(const char* data, size_t len, std::vector<char>& output) {
    size_t pos = 0;
    
    while (pos < len) {
        char msgType = data[pos];
        pos++;
        
        if (pos + 4 > len) break;
        
        uint32_t msgLen;
        memcpy(&msgLen, data + pos, 4);
        msgLen = ntohl(msgLen);
        pos += 4;
        
        if (msgType == 'D') {  // DataRow
            size_t dataLen = msgLen - 4;
            if (dataLen > 0) {
                output.insert(output.end(), data + pos, data + pos + dataLen);
            }
            pos += dataLen;
        } else {
            pos += (msgLen - 4);
        }
    }
}

void Worker::workerLoop(std::stop_token st) {
    RawKernel* kernel = manager->getKernel(connectionId);
    if (!kernel) {
        return;
    }
    
    static bool startupDone = false;
    
    while (!st.stop_requested() && isRunning.load()) {
        auto ticket = ticketSystem->acquireTicket(1000);
        if (!ticket) continue;
        
        if (!manager->isConnectionAlive(connectionId)) {
            ticket->failed = true;
            ticket->errorMessage = "Conexão morta";
            ticketSystem->returnTicket(ticket);
            continue;
        }
        
        // Startup once
        if (!startupDone) {
            size_t packetLen;
            char* packet = buildStartupPacket("postgres", "postgres", packetLen);
            if (!packet) {
                ticket->failed = true;
                ticket->errorMessage = "Falha ao construir startup";
                ticketSystem->returnTicket(ticket);
                continue;
            }
            
            int err;
            if (!kernel->sendToA(packet, packetLen, err)) {
                ticket->failed = true;
                ticket->errorMessage = "Startup falhou";
                ticketSystem->returnTicket(ticket);
                continue;
            }
            startupDone = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Send query
        std::string query(ticket->requestData.data(), ticket->requestData.size());
        size_t packetLen;
        char* packet = buildQueryPacket(query, packetLen);
        
        if (!packet) {
            ticket->failed = true;
            ticket->errorMessage = "Falha ao construir query";
            ticketSystem->returnTicket(ticket);
            continue;
        }
        
        int err;
        if (!kernel->sendToA(packet, packetLen, err)) {
            ticket->failed = true;
            ticket->errorMessage = "Envio falhou";
            ticketSystem->returnTicket(ticket);
            continue;
        }
        
        // Receive response
        ticket->responseData.clear();
        
        kernel->onFromA([&ticket](const char* data, size_t len) {
            ticket->responseData.insert(ticket->responseData.end(), data, data + len);
        });
        
        // Wait for response (timeout 30s)
        auto start = std::chrono::steady_clock::now();
        while (!st.stop_requested() && 
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start).count() < 30000) {
            if (!ticket->responseData.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        kernel->onFromA(nullptr);
        ticketSystem->returnTicket(ticket);
    }
}

bool Worker::start() {
    if (!manager->isConnectionAlive(connectionId)) {
        return false;
    }
    
    workerThread = std::jthread([this](std::stop_token st) {
        workerLoop(st);
    });
    
    return true;
}

void Worker::stop() {
    isRunning.store(false);
    if (workerThread.joinable()) {
        workerThread.request_stop();
        workerThread.join();
    }
}

int Worker::getWorkerId() const { return workerId; }
int Worker::getConnectionId() const { return connectionId; }
bool Worker::isAlive() const { return isRunning.load() && manager->isConnectionAlive(connectionId); }