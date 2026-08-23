#include <vector>
#include <atomic>
#include <thread>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

struct ExternalBufferSlot {
    int sockFd = -1;
    bool active = false;

    // Endereços de memória recebidos do script inicializador
    uint8_t* inBuf = nullptr;
    size_t inCap = 0;

    uint8_t* outBuf = nullptr;
    size_t outCap = 0;

    // Índices atômicos para controle do Ring Buffer sobre o ponteiro externo
    std::atomic<size_t> outWrite{0}, outRead{0};
    std::atomic<size_t> inWrite{0}, inRead{0};
};

class ExternalBufferSocketManager {
public:
    explicit ExternalBufferSocketManager(size_t maxConns = 10) 
        : maxConns_(maxConns), slots_(maxConns) {}

    ~ExternalBufferSocketManager() { stop(); }

    // Registra a conexão e vincula os endereços de memória externos fornecidos pelo script
    bool connectSlot(size_t slot, const char* socketPath, 
                     uint8_t* inMemoryPtr, size_t inCapacity,
                     uint8_t* outMemoryPtr, size_t outCapacity) {
        if (slot >= maxConns_ || !inMemoryPtr || !outMemoryPtr) return false;
        
        auto& s = slots_[slot];

        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) return false;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPath, sizeof(addr.sun_path) - 1);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return false;
        }

        s.sockFd = fd;
        s.inBuf = inMemoryPtr;
        s.inCap = inCapacity;
        s.outBuf = outMemoryPtr;
        s.outCap = outCapacity;

        s.outWrite.store(0); s.outRead.store(0);
        s.inWrite.store(0);  s.inRead.store(0);
        s.active = true;
        return true;
    }

    void start() {
        running_.store(true);
        workerThread_ = std::jthread(&ExternalBufferSocketManager::eventLoop, this);
    }

    void stop() {
        running_.store(false);
        if (workerThread_.joinable()) workerThread_.join();
        for (auto& s : slots_) {
            if (s.sockFd >= 0) close(s.sockFd);
            s.active = false;
        }
    }

private:
    void eventLoop() {
        std::vector<struct pollfd> pfds(maxConns_);
        std::vector<size_t> activeSlots(maxConns_);

        while (running_.load(std::memory_order_relaxed)) {
            size_t nfds = 0;

            for (size_t i = 0; i < maxConns_; ++i) {
                auto& s = slots_[i];
                if (!s.active || s.sockFd < 0 || !s.inBuf || !s.outBuf) continue;

                pfds[nfds].fd = s.sockFd;
                pfds[nfds].events = 0;

                // Verifica se há dados no buffer de saída para enviar ao socket
                size_t outW = s.outWrite.load(std::memory_order_acquire);
                size_t outR = s.outRead.load(std::memory_order_acquire);
                if (outW != outR) pfds[nfds].events |= POLLOUT;

                // Verifica se há espaço no buffer de entrada para ler do socket
                size_t inW = s.inWrite.load(std::memory_order_acquire);
                size_t inR = s.inRead.load(std::memory_order_acquire);
                size_t freeIn = s.inCap - 1 - ((inW - inR + s.inCap) % s.inCap);
                if (freeIn > 0) pfds[nfds].events |= POLLIN;

                activeSlots[nfds] = i;
                nfds++;
            }

            if (nfds == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }

            int ret = poll(pfds.data(), nfds, 10);
            if (ret <= 0) continue;

            for (size_t i = 0; i < nfds; ++i) {
                size_t slotIdx = activeSlots[i];
                auto& s = slots_[slotIdx];

                if (pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    s.active = false;
                    continue;
                }

                // Efetua a escrita no socket a partir do buffer de saída fornecido
                if (pfds[i].revents & POLLOUT) {
                    size_t curR = s.outRead.load(std::memory_order_acquire);
                    size_t curW = s.outWrite.load(std::memory_order_acquire);
                    size_t avail = (curW - curR + s.outCap) % s.outCap;
                    if (avail > 0) {
                        size_t chunk = std::min(avail, s.outCap - (curR % s.outCap));
                        ssize_t n = send(s.sockFd, s.outBuf + (curR % s.outCap), chunk, MSG_DONTWAIT);
                        if (n > 0) s.outRead.store((curR + n) % s.outCap, std::memory_order_release);
                    }
                }

                // Efetua a leitura do socket direto para o ponteiro de entrada fornecido
                if (pfds[i].revents & POLLIN) {
                    size_t curW = s.inWrite.load(std::memory_order_acquire);
                    size_t curR = s.inRead.load(std::memory_order_acquire);
                    size_t freeSp = s.inCap - 1 - ((curW - curR + s.inCap) % s.inCap);
                    if (freeSp > 0) {
                        size_t chunk = std::min(freeSp, s.inCap - (curW % s.inCap));
                        ssize_t n = recv(s.sockFd, s.inBuf + (curW % s.inCap), chunk, MSG_DONTWAIT);
                        if (n > 0) s.inWrite.store((curW + n) % s.inCap, std::memory_order_release);
                    }
                }
            }
        }
    }

    size_t maxConns_;
    std::vector<ExternalBufferSlot> slots_;
    std::atomic<bool> running_{false};
    std::jthread workerThread_;
};