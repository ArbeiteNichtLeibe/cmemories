// uteis.cpp
#include "uteis.hpp"

#include <cstring>
#include <sys/stat.h>
#include <cerrno>
#include <pwd.h>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <fcntl.h>      // <-- ADICIONADO: necessário para open, O_WRONLY, etc.
#include <unistd.h>     // <-- ADICIONADO: close, read, write, lseek, etc. (opcional, mas seguro)

namespace Utils {

// ============================================================
// CONSTANTS
// ============================================================
const uint64_t MIN_GB = 5;
const uint64_t MAX_GB = 30;
const uint64_t DEFAULT_GB = 5;
const char* CONFIG_DIR  = "/etc/memorandos";
const char* CONFIG_FILE = "/etc/memorandos/config.conf";
const char* DEFAULT_CONFIG_CONTENT =
    "# Default system configuration\n"
    "environment = development\n"
    "memory = 20\n"
    "workeruser = andre\n"
    "db_host = localhost\n"
    "db_port = 5432\n"
    "db_name = memomorandos\n"
    "db_user = postgres\n"
    "db_password = \n"
    "api_prefix = /api/v1\n"
    "auth_enabled = true\n"
    "rate_limit = 100\n"
    "welcome_message = Memo Control System version 0.001 - alpha\n"
    "template_file = /home/andre/memorandos/templates.bin\n";

// ============================================================
// HELPER
// ============================================================
static void setError(char* out_error, size_t err_size, const char* message) {
    if (out_error && err_size > 0) {
        safeCopyString(out_error, err_size, message ? message : "Unknown error");
    }
}

// ============================================================
// STRING UTILITIES
// ============================================================
void safeCopyString(char* dest, size_t dest_size, const char* src) {
    if (!dest || dest_size == 0 || !src) return;
    size_t i = 0;
    while (i < dest_size - 1 && src[i]) { dest[i] = src[i]; ++i; }
    dest[i] = '\0';
}

bool isEmpty(const char* str) {
    return (!str || str[0] == '\0');
}

// ============================================================
// FILE GUARD
// ============================================================
FileGuard::FileGuard(const char* path, int flags, mode_t mode)
    : fd_(open(path, flags, mode)) {}

FileGuard::FileGuard(int fd) : fd_(fd) {}

FileGuard::~FileGuard() { if (fd_ >= 0) close(fd_); }

FileGuard::FileGuard(FileGuard&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

FileGuard& FileGuard::operator=(FileGuard&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

bool FileGuard::isValid() const { return fd_ >= 0; }
int  FileGuard::fd()      const { return fd_; }

bool FileGuard::getSize(size_t& out_size) const {
    if (fd_ < 0) return false;
    struct stat st;
    if (fstat(fd_, &st) == -1) return false;
    out_size = st.st_size;
    return true;
}

int FileGuard::release() { int fd = fd_; fd_ = -1; return fd; }
void FileGuard::reset(int fd) { if (fd_ >= 0) close(fd_); fd_ = fd; }

ssize_t FileGuard::read(void* buffer, size_t count) const {
    if (fd_ < 0 || !buffer) { errno = EBADF; return -1; }
    char* ptr = static_cast<char*>(buffer);
    size_t total = 0;
    while (total < count) {
        ssize_t bytes = ::read(fd_, ptr + total, count - total);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (bytes == 0) break;
        total += bytes;
    }
    return total;
}

ssize_t FileGuard::write(const void* buffer, size_t count) const {
    if (fd_ < 0 || !buffer) { errno = EBADF; return -1; }
    const char* ptr = static_cast<const char*>(buffer);
    size_t total = 0;
    while (total < count) {
        ssize_t bytes = ::write(fd_, ptr + total, count - total);
        if (bytes <= 0) {
            if (bytes < 0 && errno == EINTR) continue;
            return -1;
        }
        total += bytes;
    }
    return total;
}

off_t FileGuard::seek(off_t offset, int whence) const {
    if (fd_ < 0) { errno = EBADF; return -1; }
    return lseek(fd_, offset, whence);
}

// ============================================================
// FILE & DIRECTORY
// ============================================================
bool fileExists(const char* path) {
    if (isEmpty(path)) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool isDirectory(const char* path) {
    if (isEmpty(path)) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool isAbsolutePath(const char* path) {
    return !isEmpty(path) && path[0] == '/';
}

bool getDirectoryPath(const char* filepath, char* dest, size_t dest_size) {
    if (isEmpty(filepath) || !dest || dest_size == 0) return false;
    const char* last = strrchr(filepath, '/');
    if (!last) { safeCopyString(dest, dest_size, "."); return true; }
    size_t len = last - filepath;
    if (len == 0) { safeCopyString(dest, dest_size, "/"); return true; }
    if (len >= dest_size) return false;
    for (size_t i = 0; i < len; ++i) dest[i] = filepath[i];
    dest[len] = '\0';
    return true;
}

bool getFileName(const char* filepath, char* dest, size_t dest_size) {
    if (isEmpty(filepath) || !dest || dest_size == 0) return false;
    const char* last = strrchr(filepath, '/');
    const char* name = last ? last + 1 : filepath;
    if (isEmpty(name)) return false;
    safeCopyString(dest, dest_size, name);
    return true;
}

bool joinPath(const char* base, const char* component, char* dest, size_t dest_size) {
    if (isEmpty(base) || isEmpty(component) || !dest || dest_size == 0) return false;
    if (isAbsolutePath(component)) { safeCopyString(dest, dest_size, component); return true; }
    size_t base_len = strlen(base);
    size_t comp_len = strlen(component);
    if (base_len + 1 + comp_len + 1 > dest_size) return false;
    for (size_t i = 0; i < base_len; ++i) dest[i] = base[i];
    if (base[base_len - 1] != '/') dest[base_len++] = '/';
    for (size_t i = 0; i < comp_len; ++i) dest[base_len + i] = component[i];
    dest[base_len + comp_len] = '\0';
    return true;
}

bool createDirectory(const char* path, mode_t mode, char* out_error, size_t err_size) {
    if (isEmpty(path)) { setError(out_error, err_size, "Path is empty"); return false; }
    if (isDirectory(path)) return true;
    if (mkdir(path, mode) == 0) return true;
    if (errno == ENOENT) {
        char parent[512];
        if (getDirectoryPath(path, parent, sizeof(parent)) &&
            createDirectory(parent, mode, out_error, err_size) &&
            mkdir(path, mode) == 0) {
            return true;
        }
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "Failed to create directory '%s': %s", path, strerror(errno));
    setError(out_error, err_size, msg);
    return false;
}

// ============================================================
// SYSTEM & CONFIGURATION
// ============================================================
bool isRoot() { return geteuid() == 0; }

bool parseMemoryArg(const char* arg, uint64_t& gigabytes, char* out_error, size_t err_size) {
    if (!arg) { setError(out_error, err_size, "Argument is null"); return false; }
    const char* prefix = "--memory=";
    size_t plen = strlen(prefix);
    if (strncmp(arg, prefix, plen) != 0) {
        setError(out_error, err_size, "Invalid argument prefix");
        return false;
    }
    const char* val = arg + plen;
    size_t vlen = strlen(val);
    if (vlen < 3) { setError(out_error, err_size, "Invalid memory value format"); return false; }
    char last1 = val[vlen-1], last2 = val[vlen-2];
    size_t num_len = vlen;
    if ((last1 == 'b' || last1 == 'B') && (last2 == 'g' || last2 == 'G')) num_len = vlen - 2;
    else { setError(out_error, err_size, "Memory argument must end with Gb or GB"); return false; }
    if (num_len == 0) { setError(out_error, err_size, "Missing numeric value"); return false; }
    char num_str[32];
    size_t pos = 0;
    for (size_t i = 0; i < num_len && pos < sizeof(num_str)-1; ++i) {
        if (val[i] >= '0' && val[i] <= '9') num_str[pos++] = val[i];
        else { setError(out_error, err_size, "Non-numeric characters"); return false; }
    }
    num_str[pos] = '\0';
    if (pos == 0) { setError(out_error, err_size, "Empty memory value"); return false; }
    char* endptr;
    unsigned long long value = strtoull(num_str, &endptr, 10);
    if (endptr == num_str || *endptr) {
        setError(out_error, err_size, "Failed to parse number");
        return false;
    }
    if (value == 0) { setError(out_error, err_size, "Memory value cannot be zero"); return false; }
    gigabytes = (value < MIN_GB) ? MIN_GB : (value > MAX_GB ? MAX_GB : value);
    return true;
}

void printHelp(const char* prog) {
    if (!prog) prog = "program";
    std::cout << "Usage: " << prog << " [--memory=XXGb]\n"
              << "  --memory=XXGb          Total memory (" << MIN_GB << "-" << MAX_GB << " Gb, default " << DEFAULT_GB << "Gb)\n"
              << "  --help                Show this help\n";
}

bool createConfigDirectory(char* out_error, size_t err_size) {
    if (isDirectory(CONFIG_DIR)) return true;
    return createDirectory(CONFIG_DIR, 0755, out_error, err_size);
}

bool configFileExists() { return fileExists(CONFIG_FILE); }

bool createDefaultConfig(char* out_error, size_t err_size) {
    if (!createConfigDirectory(out_error, err_size)) return false;
    FileGuard fg(CONFIG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!fg.isValid()) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to open config file '%s': %s", CONFIG_FILE, strerror(errno));
        setError(out_error, err_size, msg);
        return false;
    }
    size_t len = strlen(DEFAULT_CONFIG_CONTENT);
    if (fg.write(DEFAULT_CONFIG_CONTENT, len) != (ssize_t)len) {
        setError(out_error, err_size, "Failed to write default config");
        return false;
    }
    return true;
}

bool readConfigValue(const char* key, char* out_value, size_t out_size,
                     char* out_error, size_t err_size) {
    if (isEmpty(key) || !out_value || out_size == 0) {
        setError(out_error, err_size, "Invalid parameters");
        return false;
    }
    FileGuard fg(CONFIG_FILE, O_RDONLY);
    if (!fg.isValid()) {
        setError(out_error, err_size, "Cannot open config file");
        return false;
    }
    char line[256];
    size_t line_pos = 0;
    char ch;
    while (fg.read(&ch, 1) == 1) {
        if (ch == '\n' || line_pos >= sizeof(line)-1) {
            line[line_pos] = '\0';
            char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p != '#' && *p != '\0') {
                size_t klen = strlen(key);
                if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
                    char* val = p + klen + 1;
                    while (*val == ' ' || *val == '\t') ++val;
                    char* end = val + strlen(val) - 1;
                    while (end > val && (*end == ' ' || *end == '\t' || *end == '\r')) --end;
                    size_t vlen = end - val + 1;
                    if (vlen < out_size) {
                        for (size_t i = 0; i < vlen; ++i) out_value[i] = val[i];
                        out_value[vlen] = '\0';
                        return true;
                    } else {
                        setError(out_error, err_size, "Buffer too small for key value");
                        return false;
                    }
                }
            }
            line_pos = 0;
        } else {
            line[line_pos++] = ch;
        }
    }
    setError(out_error, err_size, "Key not found in config file");
    return false;
}

bool dropToUser(const char* username, char* out_error, size_t err_size) {
    if (isEmpty(username)) {
        setError(out_error, err_size, "Username is empty");
        return false;
    }
    struct passwd* pw = getpwnam(username);
    if (!pw) {
        char msg[256];
        snprintf(msg, sizeof(msg), "User '%s' not found", username);
        setError(out_error, err_size, msg);
        return false;
    }
    if (setgid(pw->pw_gid) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "setgid failed: %s", strerror(errno));
        setError(out_error, err_size, msg);
        return false;
    }
    if (setuid(pw->pw_uid) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "setuid failed: %s", strerror(errno));
        setError(out_error, err_size, msg);
        return false;
    }
    return true;
}

} // namespace Utils