#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mistercast {
// Groovy_MiSTer api/groovymister.h, GM_JOY_* (see docs/mpv-controller.md).
namespace controller_button {
constexpr std::uint16_t Right = 1 << 0, Left = 1 << 1;
constexpr std::uint16_t Down = 1 << 2, Up = 1 << 3;
constexpr std::uint16_t Button1 = 1 << 4;
constexpr std::uint16_t LeftShoulder = 1 << 10, RightShoulder = 1 << 11;
}
struct ControllerPacket {
    std::uint32_t frame{};
    std::uint8_t order{};
    std::array<std::uint16_t, 2> buttons{};
};
std::optional<ControllerPacket> decodeControllerPacket(std::span<const std::uint8_t> bytes);
enum class PlaybackAction { Pause, SeekBack, SeekForward, VolumeUp, VolumeDown, Previous, Next };
std::string_view mpvCommand(PlaybackAction action);
class ControllerInput {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto RepeatDelay = std::chrono::milliseconds(400);
    static constexpr auto RepeatInterval = std::chrono::milliseconds(150);
    // Upstream sends idle snapshots only every 2700 blits (45 s at 60 Hz).
    static constexpr auto InputTimeout = std::chrono::seconds(60);
    // Returns false for duplicates/out-of-order events. No state changes on rejection.
    bool update(const ControllerPacket& packet, Clock::time_point now,
                std::vector<PlaybackAction>& actions);
    std::vector<PlaybackAction> tick(Clock::time_point now);
    void reset();
    void suppressHeld() { suppressed_ = held_; nextRepeat_ = {}; }
    const std::array<std::uint16_t, 2>& held() const { return held_; }
    const std::array<std::uint16_t, 2>& pressed() const { return pressed_; }
    const std::array<std::uint16_t, 2>& released() const { return released_; }
private:
    void expire(Clock::time_point now);
    std::array<std::uint16_t, 2> held_{}, pressed_{}, released_{}, suppressed_{};
    std::array<Clock::time_point, 7> nextRepeat_{};
    ControllerPacket last_{};
    Clock::time_point lastReceived_{};
    bool haveSequence_{}, recovering_{};
};
} // namespace mistercast
