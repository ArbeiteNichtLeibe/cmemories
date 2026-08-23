// socket_reader.hpp
#ifndef SOCKET_READER_HPP
#define SOCKET_READER_HPP

#include <atomic>
#include <thread>
#include <cstddef>
#include <cstdint>
#include "../../memorymanager/include/memory_manager_thread.hpp"

class SocketReader {
public:
    SocketReader(memorymanager::MemoryManagerThread* mm);
    ~SocketReader();

    bool connect(const char* socketPath, char* outError, size_t errSize);
    bool start(char* outError, size_t errSize);
    void stop();

    size_t writeOut(const uint8_t* data, size_t len);
    size_t readIn(uint8_t* outData, size_t maxLen);
    size_t outSpace() const;
    size_t inData() const;

private:
    bool allocateBuffer(size_t sizeBytes, uint64_t& outThreadId, uint8_t*& outPtr,
                        char* outError, size_t errSize);
    void freeBuffer(uint64_t threadId);
    void threadLoop();

    memorymanager::MemoryManagerThread* mm_;
    int sockFd_;

    uint8_t* outBuf_;
    size_t   outSize_;
    uint64_t outThreadId_;

    uint8_t* inBuf_;
    size_t   inSize_;
    uint64_t inThreadId_;

    std::atomic<size_t> outWrite_;
    std::atomic<size_t> outRead_;
    std::atomic<size_t> inWrite_;
    std::atomic<size_t> inRead_;

    std::jthread thread_;
    std::atomic<bool> running_;

    static constexpr size_t BUFFER_SIZE = 6 * 1024 * 1024; // 6 MB
};

#endif // SOCKET_READER_HPP