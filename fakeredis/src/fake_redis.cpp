#include "../include/fake_redis.hpp"
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <string_view>

namespace FakeRedis {

// ------------------------------------------------------------------
// Singleton
// ------------------------------------------------------------------
FakeRedis& FakeRedis::getInstance() {
    static FakeRedis instance;
    return instance;
}

// ------------------------------------------------------------------
// Constructor / Destructor
// ------------------------------------------------------------------
FakeRedis::FakeRedis()
    : m_mm(nullptr)
    , m_token(0)
    , m_startBlock(0)
    , m_allocatedBlocks(0)
    , m_memoryBase(nullptr)
    , m_initialized(false)
    , m_totalKeys(0)
    , m_opsSet(0), m_opsGet(0), m_opsDel(0), m_opsExpired(0), m_opsFail(0)
    , m_seed(123456789ULL) {
    for (auto& r : m_regions) {
        r.base = nullptr;
        r.bitmap = nullptr;
        r.numBlocks = 0;
        r.blockSize = 0;
        r.freeBlocks = 0;
        r.usedBlocks = 0;
    }
    for (auto& c : m_blocksPerClass) {
        c = 0;
    }
}

FakeRedis::~FakeRedis() {
    finalize();
}

// ------------------------------------------------------------------
// xorshift64 PRNG
// ------------------------------------------------------------------
uint64_t FakeRedis::xorshift64() {
    uint64_t x = m_seed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    m_seed = x;
    return x;
}

// ------------------------------------------------------------------
// Initialization
// ------------------------------------------------------------------
bool FakeRedis::initialize(memorymanager::MemoryManagerThread* mm, uint32_t token,
                            char* outError, size_t errSize) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) {
        return true;
    }

    if (!mm) {
        if (outError) {
            snprintf(outError, errSize, "MemoryManagerThread is null");
        }
        return false;
    }
    m_mm = mm;
    m_token = token;

    constexpr uint32_t BLOCKS_NEEDED = 2048; // 2 GB = 2048 * 1MB blocks
    uint32_t startBlock = 0;
    void* startAddr = nullptr;
    void* endAddr = nullptr;
    char err[256] = {0};

    if (!m_mm->allocate(static_cast<uint64_t>(token), BLOCKS_NEEDED,
                        startBlock, startAddr, endAddr, err)) {
        if (outError) {
            snprintf(outError, errSize, "Failed to allocate 2 GB: %s", err);
        }
        return false;
    }

    m_memoryBase = startAddr;
    m_startBlock = startBlock;
    m_allocatedBlocks = BLOCKS_NEEDED;

    uint8_t* base = static_cast<uint8_t*>(m_memoryBase);
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        if (!initRegion(m_regions[i], base + i * REGION_SIZE,
                        BLOCK_SIZES[i], outError, errSize)) {
            m_mm->free(static_cast<uint64_t>(token), err);
            m_memoryBase = nullptr;
            return false;
        }
        m_blocksPerClass[i] = m_regions[i].numBlocks;
    }

    m_initialized = true;
    fprintf(stdout, "[FakeRedis] Successfully initialized (2 GB via MemoryManagerThread)\n");
    return true;
}

// ------------------------------------------------------------------
// Finalization
// ------------------------------------------------------------------
void FakeRedis::finalize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized && m_mm && m_memoryBase) {
        printStats();
        char err[128] = {0};
        if (!m_mm->free(static_cast<uint64_t>(m_token), err)) {
            fprintf(stderr, "[FakeRedis] Warning: memory free failed: %s\n", err);
        }
        m_memoryBase = nullptr;
        for (auto& r : m_regions) {
            r.base = nullptr;
            r.bitmap = nullptr;
            r.numBlocks = 0;
            r.blockSize = 0;
            r.freeBlocks = 0;
            r.usedBlocks = 0;
        }
        for (auto& c : m_blocksPerClass) {
            c = 0;
        }
        m_initialized = false;
        fprintf(stdout, "[FakeRedis] Finalized\n");
    }
}

// ------------------------------------------------------------------
// Helper: Initialize Region with Bitmaps and Alignment
// ------------------------------------------------------------------
bool FakeRedis::initRegion(Region& reg, uint8_t* base, size_t blockSize,
                           char* outError, size_t errSize) {
    reg.base = base;
    reg.blockSize = blockSize;

    size_t maxPossibleBlocks = REGION_SIZE / blockSize;
    size_t bitmapBytes = (maxPossibleBlocks + 7) / 8;
    reg.bitmap = base;

    // Zeroing bitmap using std::fill_n instead of std::memset
    std::fill_n(reg.bitmap, bitmapBytes, static_cast<uint8_t>(0));

    // Align data start boundary to alignof(std::max_align_t)
    uintptr_t rawDataStart = reinterpret_cast<uintptr_t>(base + bitmapBytes);
    constexpr size_t alignment = alignof(std::max_align_t);
    uintptr_t alignedDataStart = (rawDataStart + (alignment - 1)) & ~(alignment - 1);

    size_t metadataSize = alignedDataStart - reinterpret_cast<uintptr_t>(base);
    if (metadataSize >= REGION_SIZE) {
        if (outError) {
            snprintf(outError, errSize, "Region metadata exceeds total region size");
        }
        return false;
    }

    size_t available = REGION_SIZE - metadataSize;
    size_t realBlocks = available / blockSize;

    if (realBlocks == 0) {
        if (outError) {
            snprintf(outError, errSize, "Region too small for block size %zu", blockSize);
        }
        return false;
    }

    reg.numBlocks = realBlocks;
    reg.freeBlocks = realBlocks;
    reg.usedBlocks = 0;
    return true;
}

// ------------------------------------------------------------------
// Choose Region
// ------------------------------------------------------------------
int FakeRedis::chooseRegion(size_t valueLen) const {
    const size_t total = sizeof(BlockHeader) + valueLen;
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        if (total <= BLOCK_SIZES[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ------------------------------------------------------------------
// Bitmap Access
// ------------------------------------------------------------------
bool FakeRedis::isBlockFree(const Region& reg, size_t blockIndex) const {
    size_t wordIdx = blockIndex / 64;
    size_t bitIdx = blockIndex % 64;
    const uint64_t* bits = reinterpret_cast<const uint64_t*>(reg.bitmap);
    return (bits[wordIdx] & (1ULL << bitIdx)) == 0;
}

void FakeRedis::setBlockUsed(Region& reg, size_t blockIndex, bool used) {
    size_t wordIdx = blockIndex / 64;
    size_t bitIdx = blockIndex % 64;
    uint64_t* bits = reinterpret_cast<uint64_t*>(reg.bitmap);
    if (used) {
        bits[wordIdx] |= (1ULL << bitIdx);
        reg.freeBlocks--;
        reg.usedBlocks++;
    } else {
        bits[wordIdx] &= ~(1ULL << bitIdx);
        reg.freeBlocks++;
        reg.usedBlocks--;
    }
}

// ------------------------------------------------------------------
// Block Offset Helper
// ------------------------------------------------------------------
BlockHeader* FakeRedis::getBlock(const Region& reg, size_t blockIndex) const {
    size_t maxPossibleBlocks = REGION_SIZE / reg.blockSize;
    size_t bitmapBytes = (maxPossibleBlocks + 7) / 8;
    uintptr_t rawDataStart = reinterpret_cast<uintptr_t>(reg.base + bitmapBytes);
    constexpr size_t alignment = alignof(std::max_align_t);
    uintptr_t alignedDataStart = (rawDataStart + (alignment - 1)) & ~(alignment - 1);

    uint8_t* blockPtr = reinterpret_cast<uint8_t*>(alignedDataStart) + (blockIndex * reg.blockSize);
    return reinterpret_cast<BlockHeader*>(blockPtr);
}

// ------------------------------------------------------------------
// Expiration Check
// ------------------------------------------------------------------
bool FakeRedis::isExpired(const BlockHeader* block) const {
    if (block->expireAt == 0) return false;
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    return static_cast<uint64_t>(now) >= block->expireAt;
}

// ------------------------------------------------------------------
// Free Block
// ------------------------------------------------------------------
void FakeRedis::freeBlock(Region& reg, size_t blockIndex) {
    setBlockUsed(reg, blockIndex, false);
    BlockHeader* block = getBlock(reg, blockIndex);
    block->magic = 0;
    if (m_totalKeys > 0) {
        m_totalKeys--;
    }
}

// ------------------------------------------------------------------
// SET
// ------------------------------------------------------------------
int64_t FakeRedis::set(std::string_view value, uint32_t ttlMs,
                       char* outError, size_t errSize) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        if (outError) snprintf(outError, errSize, "Not initialized");
        return -1;
    }

    if (ttlMs == 0 || ttlMs > MAX_TTL_MS) {
        if (outError) snprintf(outError, errSize, "Invalid TTL");
        m_opsFail++;
        return -1;
    }

    int regionIdx = chooseRegion(value.size());
    if (regionIdx < 0) {
        if (outError) snprintf(outError, errSize, "Value too large (>16MB)");
        m_opsFail++;
        return -1;
    }

    Region& reg = m_regions[regionIdx];
    if (reg.freeBlocks == 0) {
        if (outError) snprintf(outError, errSize, "Region out of memory");
        m_opsFail++;
        return -1;
    }

    constexpr int MAX_TRIES = 3;
    for (int tryNum = 0; tryNum < MAX_TRIES; ++tryNum) {
        size_t id = xorshift64() % reg.numBlocks;
        for (int attempt = 0; attempt < 2; ++attempt) {
            size_t blockIndex = (id + attempt) % reg.numBlocks;
            if (isBlockFree(reg, blockIndex)) {
                BlockHeader* block = getBlock(reg, blockIndex);
                block->magic = 0xDEADBEEF;
                auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count();
                block->expireAt = static_cast<uint64_t>(now + ttlMs);
                block->valueLen = static_cast<uint32_t>(value.size());

                char* data = reinterpret_cast<char*>(block + 1);
                std::copy_n(value.data(), value.size(), data);

                setBlockUsed(reg, blockIndex, true);
                m_totalKeys++;
                m_opsSet++;

                int64_t globalId = 0;
                for (int c = 0; c < regionIdx; ++c) {
                    globalId += static_cast<int64_t>(m_blocksPerClass[c]);
                }
                globalId += static_cast<int64_t>(blockIndex);
                return globalId;
            }
        }
    }

    if (outError) snprintf(outError, errSize, "Could not allocate block after %d retries", MAX_TRIES);
    m_opsFail++;
    return -1;
}

// ------------------------------------------------------------------
// GET
// ------------------------------------------------------------------
bool FakeRedis::get(int64_t id, char* outBuffer, size_t bufferSize,
                    char* outError, size_t errSize) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        if (outError) snprintf(outError, errSize, "Not initialized");
        return false;
    }

    if (id < 0) {
        if (outError) snprintf(outError, errSize, "Invalid ID");
        return false;
    }

    size_t classOffset = 0;
    int regionIdx = -1;
    size_t localId = 0;
    for (size_t r = 0; r < NUM_CLASSES; ++r) {
        if (static_cast<size_t>(id) < classOffset + m_blocksPerClass[r]) {
            regionIdx = static_cast<int>(r);
            localId = static_cast<size_t>(id) - classOffset;
            break;
        }
        classOffset += m_blocksPerClass[r];
    }
    if (regionIdx < 0) {
        if (outError) snprintf(outError, errSize, "ID out of range");
        return false;
    }

    Region& reg = m_regions[regionIdx];
    if (localId >= reg.numBlocks) {
        if (outError) snprintf(outError, errSize, "ID out of class bounds");
        return false;
    }

    BlockHeader* block = getBlock(reg, localId);
    if (block->magic != 0xDEADBEEF) {
        return false;
    }

    if (isExpired(block)) {
        freeBlock(reg, localId);
        m_opsExpired++;
        if (outError) snprintf(outError, errSize, "Key expired");
        return false;
    }

    size_t valLen = block->valueLen;
    if (valLen >= bufferSize) {
        if (outError) snprintf(outError, errSize, "Buffer too small (requires %zu)", valLen + 1);
        return false;
    }

    char* data = reinterpret_cast<char*>(block + 1);
    std::copy_n(data, valLen, outBuffer);
    outBuffer[valLen] = '\0';
    m_opsGet++;
    return true;
}

// ------------------------------------------------------------------
// DEL
// ------------------------------------------------------------------
bool FakeRedis::del(int64_t id, char* outError, size_t errSize) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) {
        if (outError) snprintf(outError, errSize, "Not initialized");
        return false;
    }

    size_t classOffset = 0;
    int regionIdx = -1;
    size_t localId = 0;
    for (size_t r = 0; r < NUM_CLASSES; ++r) {
        if (static_cast<size_t>(id) < classOffset + m_blocksPerClass[r]) {
            regionIdx = static_cast<int>(r);
            localId = static_cast<size_t>(id) - classOffset;
            break;
        }
        classOffset += m_blocksPerClass[r];
    }
    if (regionIdx < 0) return false;

    Region& reg = m_regions[regionIdx];
    if (localId >= reg.numBlocks) return false;

    BlockHeader* block = getBlock(reg, localId);
    if (block->magic != 0xDEADBEEF) return false;

    freeBlock(reg, localId);
    m_opsDel++;
    return true;
}

// ------------------------------------------------------------------
// EXISTS
// ------------------------------------------------------------------
bool FakeRedis::exists(int64_t id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return false;

    size_t classOffset = 0;
    int regionIdx = -1;
    size_t localId = 0;
    for (size_t r = 0; r < NUM_CLASSES; ++r) {
        if (static_cast<size_t>(id) < classOffset + m_blocksPerClass[r]) {
            regionIdx = static_cast<int>(r);
            localId = static_cast<size_t>(id) - classOffset;
            break;
        }
        classOffset += m_blocksPerClass[r];
    }
    if (regionIdx < 0) return false;

    const Region& reg = m_regions[regionIdx];
    if (localId >= reg.numBlocks) return false;

    const BlockHeader* block = getBlock(reg, localId);
    return (block->magic == 0xDEADBEEF && !isExpired(block));
}

// ------------------------------------------------------------------
// Statistics
// ------------------------------------------------------------------
FakeRedis::Stats FakeRedis::getStats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    Stats s;
    s.totalKeys = m_totalKeys;
    s.opsSet = m_opsSet;
    s.opsGet = m_opsGet;
    s.opsDel = m_opsDel;
    s.opsExpired = m_opsExpired;
    s.opsFail = m_opsFail;
    return s;
}

void FakeRedis::printStats() {
    Stats s = getStats();
    fprintf(stdout, "\n========== FAKEREDIS STATS ==========\n");
    fprintf(stdout, "Active Keys          : %zu\n", s.totalKeys);
    fprintf(stdout, "SET Operations       : %zu\n", s.opsSet);
    fprintf(stdout, "GET Operations       : %zu\n", s.opsGet);
    fprintf(stdout, "DEL Operations       : %zu\n", s.opsDel);
    fprintf(stdout, "Expired Keys         : %zu\n", s.opsExpired);
    fprintf(stdout, "Failures (General)   : %zu\n", s.opsFail);
    fprintf(stdout, "========================================\n\n");
}

} // namespace FakeRedis