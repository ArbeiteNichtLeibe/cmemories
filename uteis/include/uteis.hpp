// uteis.hpp
#pragma once

#include <cstdint>
#include <sys/types.h>
#include <unistd.h>

namespace Utils {

// ============================================================
// CONSTANTS
// ============================================================
extern const uint64_t MIN_GB;
extern const uint64_t MAX_GB;
extern const uint64_t DEFAULT_GB;
extern const char*   CONFIG_DIR;
extern const char*   CONFIG_FILE;
extern const char*   DEFAULT_CONFIG_CONTENT;

// ============================================================
// STRING UTILITIES
// ============================================================
void safeCopyString(char* dest, size_t dest_size, const char* src);
bool isEmpty(const char* str);

// ============================================================
// RAII FILE GUARD
// ============================================================
class FileGuard {
public:
    explicit FileGuard(const char* path, int flags, mode_t mode = 0644);
    explicit FileGuard(int fd = -1);
    ~FileGuard();

    FileGuard(FileGuard&& other) noexcept;
    FileGuard& operator=(FileGuard&& other) noexcept;

    bool isValid() const;
    int  fd()      const;
    bool getSize(size_t& out_size) const;
    int  release();
    void reset(int fd = -1);

    ssize_t read (void* buffer, size_t count) const;
    ssize_t write(const void* buffer, size_t count) const;
    off_t   seek(off_t offset, int whence) const;

private:
    int fd_;
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
};

// ============================================================
// FILE & DIRECTORY UTILITIES
// ============================================================
bool fileExists       (const char* path);
bool isDirectory      (const char* path);
bool isAbsolutePath   (const char* path);
bool getDirectoryPath (const char* filepath, char* dest, size_t dest_size);
bool getFileName      (const char* filepath, char* dest, size_t dest_size);
bool joinPath         (const char* base, const char* component, char* dest, size_t dest_size);

bool createDirectory(const char* path, mode_t mode,
                     char* out_error = nullptr, size_t err_size = 0);

// ============================================================
// SYSTEM & CONFIGURATION UTILITIES
// ============================================================
bool isRoot();

bool parseMemoryArg(const char* arg, uint64_t& gigabytes,
                    char* out_error = nullptr, size_t err_size = 0);

void printHelp(const char* prog);

bool createConfigDirectory(char* out_error = nullptr, size_t err_size = 0);
bool configFileExists();
bool createDefaultConfig(char* out_error = nullptr, size_t err_size = 0);

bool readConfigValue(const char* key, char* out_value, size_t out_size,
                     char* out_error = nullptr, size_t err_size = 0);

bool dropToUser(const char* username,
                char* out_error = nullptr, size_t err_size = 0);

} // namespace Utils