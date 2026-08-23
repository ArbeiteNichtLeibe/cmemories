#ifndef FAKE_REDIS_HPP
#define FAKE_REDIS_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string_view>
#include "../../memorymanager/include/memory_manager_thread.hpp"

namespace FakeRedis {

// ============================================================
// CONSTANTS
// ============================================================
static constexpr size_t TOTAL_MEMORY      = 2ULL * 1024ULL * 1024ULL * 1024ULL; // 2 GB
static constexpr size_t REGION_SIZE       = TOTAL_MEMORY / 5;                    // 409.6 MB each
static constexpr size_t NUM_CLASSES       = 5;
static constexpr size_t MAX_TTL_MS        = 3ULL * 24ULL * 3600ULL * 1000ULL;    // 3 days

// Block sizes per class (in bytes)
static constexpr size_t BLOCK_SIZES[NUM_CLASSES] = {
    128,           // Class 0: up to 128B
    1024,          // Class 1: up to 1KB
    8192,          // Class 2: up to 8KB
    65536,         // Class 3: up to 64KB
    16*1024*1024   // Class 4: up to 16MB
};

// ============================================================
// STRUCTURES
// ============================================================

struct BlockHeader {
    uint32_t magic;       // 0xDEADBEEF
    uint64_t expireAt;    // Timestamp in ms
    uint32_t valueLen;    // Length of value payload
};

struct Region {
    uint8_t* base;        // Region base pointer
    uint8_t* bitmap;      // Bitmap pointer inside region
    size_t numBlocks;     // Dynamic count of usable blocks
    size_t blockSize;
    size_t freeBlocks;
    size_t usedBlocks;
};

// ============================================================
// MAIN CLASS (SINGLETON)
// ============================================================

class FakeRedis {
public:
    static FakeRedis& getInstance();

    bool initialize(memorymanager::MemoryManagerThread* mm, uint32_t token,
                    char* outError = nullptr, size_t errSize = 0);
    void finalize();

    int64_t set(std::string_view value, uint32_t ttlMs,
                char* outError = nullptr, size_t errSize = 0);
    bool get(int64_t id, char* outBuffer, size_t bufferSize,
             char* outError = nullptr, size_t errSize = 0);
    bool del(int64_t id, char* outError = nullptr, size_t errSize = 0);
    bool exists(int64_t id);

    struct Stats {
        size_t totalKeys;
        size_t opsSet;
        size_t opsGet;
        size_t opsDel;
        size_t opsExpired;
        size_t opsFail;
    };
    Stats getStats();
    void printStats();

private:
    FakeRedis();
    ~FakeRedis();
    FakeRedis(const FakeRedis&) = delete;
    FakeRedis& operator=(const FakeRedis&) = delete;

    bool initRegion(Region& reg, uint8_t* base, size_t blockSize,
                    char* outError, size_t errSize);
    int chooseRegion(size_t valueLen) const;
    bool isBlockFree(const Region& reg, size_t blockIndex) const;
    void setBlockUsed(Region& reg, size_t blockIndex, bool used);
    BlockHeader* getBlock(const Region& reg, size_t blockIndex) const;
    bool isExpired(const BlockHeader* block) const;
    void freeBlock(Region& reg, size_t blockIndex);
    uint64_t xorshift64();

    memorymanager::MemoryManagerThread* m_mm;
    uint32_t m_token;
    uint32_t m_startBlock;
    uint32_t m_allocatedBlocks;
    void* m_memoryBase;
    bool m_initialized;

    Region m_regions[NUM_CLASSES];
    size_t m_blocksPerClass[NUM_CLASSES];

    size_t m_totalKeys;
    size_t m_opsSet;
    size_t m_opsGet;
    size_t m_opsDel;
    size_t m_opsExpired;
    size_t m_opsFail;

    mutable std::mutex m_mutex;
    uint64_t m_seed;
};

} // namespace FakeRedis

#endif // FAKE_REDIS_HPP