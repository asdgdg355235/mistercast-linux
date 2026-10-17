#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace mistercast {
class ControllerReceiver {
public:
    using Warning = std::function<void(const std::string&)>;
    static constexpr std::uint16_t Port = 32101;
    ~ControllerReceiver();
    // Register before the streaming CMD_INIT: upstream consumes registration there.
    bool start(const std::string& host, const std::string& ipcPath,
               Warning warning, std::string& error, std::uint16_t port = Port);
    void activate() { active_ = true; }
    void stop();
private:
    std::jthread thread_;
    std::atomic_bool active_{};
};
} // namespace mistercast
