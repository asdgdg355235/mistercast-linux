#pragma once
#include "input/ControllerInput.h"
#include <linux/input.h>
#include <span>
#include <string>
#include <vector>

namespace mistercast {
struct GamepadState {
    std::uint16_t buttons{};
    std::array<int, 4> axes{};
    std::array<std::uint8_t, 2> triggers{};
    bool operator==(const GamepadState&) const = default;
};
// Digital mode mirrors the hat onto X/Y at one-bit precision for Linux input_id.
GamepadState gamepadState(const ControllerPacket& packet, unsigned slot);
std::vector<input_event> gamepadEvents(const GamepadState& oldState,
                                     const GamepadState& state, bool analog);
class VirtualGamepad {
public:
    ~VirtualGamepad();
    VirtualGamepad() = default;
    VirtualGamepad(const VirtualGamepad&) = delete;
    VirtualGamepad& operator=(const VirtualGamepad&) = delete;
    bool create(unsigned slot, bool analog, const std::string& endpoint, std::string& error);
    bool update(const GamepadState& state, std::string& error);
    void close();
    bool exists() const { return fd_ >= 0; }
    bool analog() const { return analog_; }
    const std::string& sysname() const { return sysname_; }
private:
    int fd_{-1};
    bool analog_{};
    GamepadState state_{};
    std::string sysname_;
};
// Serial ordering and watchdog shared by both slots. No per-controller presence
// or disconnect information exists on the wire.
class GamepadInput {
public:
    using Clock = ControllerInput::Clock;
    explicit GamepadInput(std::chrono::milliseconds timeout) : timeout_(timeout) {}
    bool update(const ControllerPacket& packet, Clock::time_point now);
    bool expire(Clock::time_point now);
    bool live() const { return live_; }
    const ControllerPacket& packet() const { return last_; }
private:
    std::chrono::milliseconds timeout_;
    ControllerPacket last_{};
    Clock::time_point received_{};
    bool live_{};
};
std::chrono::milliseconds controllerTimeout(double blitsPerSecond);
} // namespace mistercast
