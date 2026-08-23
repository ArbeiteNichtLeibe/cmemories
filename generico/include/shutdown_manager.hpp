// shutdown_manager.hpp
#pragma once

#include "../../memorymanager/include/memory_manager_thread.hpp"

namespace ShutdownManager {
    extern std::atomic<bool> g_should_stop;

    void setupHandlers();
    void startMonitor();
    void shutdownSystem(memorymanager::MemoryManagerThread* memory_manager);
} // namespace ShutdownManager