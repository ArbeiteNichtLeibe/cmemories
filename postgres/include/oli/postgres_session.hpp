// postgres_session.hpp
// ATENÇÃO: Este header não possui include guards. Inclua-o apenas uma vez.

#include "socket_reader.hpp"
#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <chrono>

class PostgresSession {
public:
    PostgresSession(SocketReader& socketReader);
    ~PostgresSession();

    // Define a senha a ser usada na autenticação (deve ser chamada antes de start())
    void setPassword(const char* password);

    bool start(char* outError, size_t errSize);
    void stop();

    bool sendQuery(const char* query, size_t len, char* outError, size_t errSize);
    bool isReady() const { return state_ == State::Ready; }
    bool isError() const { return state_ == State::Error; }

private:
    enum class State {
        Startup,
        AuthWaiting,
        Ready,
        QueryPending,
        Error
    };

    struct Command {
        const uint8_t* data;
        size_t len;
    };

    void protocolLoop();
    bool sendStartupMessage(char* outError, size_t errSize);
    bool processIncoming();
    bool handleMessage(const uint8_t* msg, size_t len, size_t& consumed);
    bool sendRaw(const uint8_t* data, size_t len, char* outError, size_t errSize);

    // Autenticação
    bool sendPasswordMessage(char* outError, size_t errSize);
    bool handleAuthentication(const uint8_t* msg, size_t len, size_t& consumed);

    static constexpr size_t MAX_PENDING_COMMANDS = 16;
    Command commandQueue_[MAX_PENDING_COMMANDS];
    size_t commandHead_ = 0;
    size_t commandTail_ = 0;
    std::mutex commandMutex_;

    SocketReader& socketReader_;

    std::atomic<bool> running_;
    std::jthread workerThread_;
    std::mutex cvMutex_;
    std::condition_variable dataCV_;

    State state_;
    uint8_t readBuffer_[64 * 1024];
    size_t readLen_;

    const char* dbName_ = "postgres";
    const char* user_   = "postgres";
    char password_[64];
    bool hasPassword_ = false;

    std::chrono::steady_clock::time_point startup_time_;
};