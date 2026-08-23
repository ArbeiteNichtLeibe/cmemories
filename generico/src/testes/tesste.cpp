#include <iostream>
#include <sys/mman.h>  // Required for mmap
#include <cstring>     // Required for memset
#include <unistd.h>    // Required for standard size constants
#include <cstdlib>     // Required for exit()

void check_and_exit(const char* message) {
    perror(message);
    exit(EXIT_FAILURE);
}

int main() {
    // 1 MB of memory
    size_t size = 1024 * 1024;

    // Allocate anonymous memory:
    // - MAP_ANONYMOUS: Not backed by any file (standard for "anonymous" memory)
    // - MAP_PRIVATE: Not shared with other processes
    void* addr = mmap(NULL, size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1, 0);

    if (addr == MAP_FAILED) {
        perror("mmap failed");
        return EXIT_FAILURE;
    }

    // "Touch" the memory:
    // memset fills the region with a value, ensuring all pages are mapped.
    if (std::memset(addr, 0xAA, size) == nullptr) {
        perror("memset failed");
        munmap(addr, size);  // Clean up before exiting
        return EXIT_FAILURE;
    }

    std::cout << "Successfully allocated anonymous memory at: " << addr << std::endl;

    // munmap is technically better than just exiting to clean up immediately
    if (munmap(addr, size) == -1) {
        perror("munmap failed");
        return EXIT_FAILURE;
    }

    std::cout << "Successfully unmapped memory" << std::endl;
    return EXIT_SUCCESS;
}
