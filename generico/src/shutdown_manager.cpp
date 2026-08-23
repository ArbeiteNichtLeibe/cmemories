// shutdown_manager.cpp
#include "shutdown_manager.hpp"
#include "../include/start_tpm.hpp"
#include "json_start.hpp"
#include "../../simpleserver/include/http_server.hpp"
#include "../../jsonhandler/include/json_generator.hpp"
#include "../../postgres/include/start_connections_sql.hpp"   // <-- pool
#include <iostream>
#include <csignal>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <unistd.h>

#include "../../fakeredis/include/fake_redis.hpp"
#include "../../lerconfig/include/config.hpp"
#include "../../memorymanager/include/memory_manager_thread.hpp"

namespace ShutdownManager {

std::atomic<bool> g_should_stop{false};
static std::mutex g_signal_mutex;
static std::condition_variable g_signal_cv;
static bool g_signal_triggered = false;

static void signalHandler(int sig) {
    (void)sig;
    g_should_stop.store(true);
    {
        std::lock_guard<std::mutex> lock(g_signal_mutex);
        g_signal_triggered = true;
    }
    g_signal_cv.notify_one();
    std::cout << "\n⚠️ Signal received, initiating shutdown..." << std::endl;
}

static void monitorThread() {
    std::unique_lock<std::mutex> lock(g_signal_mutex);
    g_signal_cv.wait(lock, [] { return g_signal_triggered; });

    std::cout << "⏳ Waiting 2 seconds for clean shutdown..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << "⏰ Timeout: forcing process exit." << std::endl;
    std::fflush(stdout);
    std::fflush(stderr);
    _exit(0);
}

void setupHandlers() {
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void startMonitor() {
    std::jthread t(monitorThread);
    t.detach();
}

void shutdownSystem(memorymanager::MemoryManagerThread* memory_manager) {
    std::cout << "\n🛑 Shutting down system..." << std::endl;

    http::HttpServer& httpServer = http::HttpServer::getInstance();
    httpServer.shutdown();
    std::cout << "✅ HttpServer shut down." << std::endl;

    JSONWorker::JsonGenerator::destroyInstance();
    std::cout << "✅ JsonGenerator destroyed." << std::endl;

    TPMStart::shutdownTPM();

    // Desliga o pool de conexões PostgreSQL
    SqlConnectionManager::getInstance().shutdown();
    std::cout << "✅ PostgreSQL pool shut down." << std::endl;

    FakeRedis::FakeRedis::getInstance().finalize();

    LerConfig::Config::setMemoryManager(nullptr);

    if (memory_manager != nullptr) {
        memory_manager->shutdown();
    }

    if (unlink("/run/memorandos/jwt_chave.conf") == 0) {
        std::cout << "🗑️ Removed /run/memorandos/jwt_chave.conf" << std::endl;
    } else {
        std::cout << "ℹ️ /run/memorandos/jwt_chave.conf not found (already removed)" << std::endl;
    }
    if (unlink("/run/memorandos/discreto.conf") == 0) {
        std::cout << "🗑️ Removed /run/memorandos/discreto.conf" << std::endl;
    } else {
        std::cout << "ℹ️ /run/memorandos/discreto.conf not found (already removed)" << std::endl;
    }
    rmdir("/run/memorandos");

    std::cout << "✅ Shutdown complete." << std::endl;
}

} // namespace ShutdownManager