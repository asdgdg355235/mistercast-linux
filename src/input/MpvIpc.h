#pragma once
#include "input/ControllerInput.h"
#include <map>
#include <string>

namespace mistercast {
// Nonblocking, bounded IPC. Owned and called exclusively by the input worker.
class MpvIpc {
public:
    using Clock = ControllerInput::Clock;
    static constexpr std::size_t MaxInFlight = 16;
    static constexpr auto ReplyTimeout = std::chrono::seconds(1);
    ~MpvIpc();
    MpvIpc() = default;
    MpvIpc(const MpvIpc&) = delete;
    MpvIpc& operator=(const MpvIpc&) = delete;
    bool connect(const std::string& path, Clock::time_point now, std::string& error);
    bool service(Clock::time_point now, std::string& error);
    bool send(PlaybackAction action, Clock::time_point now, std::string& error);
    bool ready() const { return fd_ >= 0 && !connecting_; }
    void close();
private:
    int fd_{-1};
    bool connecting_{};
    Clock::time_point connectAt_{};
    std::uint64_t nextId_{1};
    std::map<std::uint64_t, Clock::time_point> pending_;
    std::string incoming_;
};
} // namespace mistercast
