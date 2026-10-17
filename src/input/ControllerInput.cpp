#include "input/ControllerInput.h"

namespace mistercast {
namespace {
struct Binding { std::uint16_t mask; PlaybackAction action; bool repeat; };
using namespace controller_button;
constexpr std::array bindings{
    Binding{Button1, PlaybackAction::Pause, false},
    Binding{Left, PlaybackAction::SeekBack, true},
    Binding{Right, PlaybackAction::SeekForward, true},
    Binding{Up, PlaybackAction::VolumeUp, true},
    Binding{Down, PlaybackAction::VolumeDown, true},
    Binding{LeftShoulder, PlaybackAction::Previous, false},
    Binding{RightShoulder, PlaybackAction::Next, false},
};
}
std::optional<ControllerPacket> decodeControllerPacket(std::span<const std::uint8_t> b)
{
    if (b.size() != 9 && b.size() != 17) return std::nullopt;
    // Do not cast packet storage to native integers: alignment and host endianness vary.
    ControllerPacket p;
    for (unsigned i = 0; i < 4; ++i) p.frame |= std::uint32_t(b[i]) << (8 * i);
    p.order = b[4];
    for (unsigned i = 0; i < 2; ++i)
        p.buttons[i] = std::uint16_t(b[5 + 2 * i]) | (std::uint16_t(b[6 + 2 * i]) << 8);
    return p; // The optional eight analog bytes have no playback bindings.
}
std::string_view mpvCommand(PlaybackAction action)
{
    switch (action) {
    case PlaybackAction::Pause: return R"(["cycle","pause"])";
    case PlaybackAction::SeekBack: return R"(["seek",-5,"relative"])";
    case PlaybackAction::SeekForward: return R"(["seek",5,"relative"])";
    case PlaybackAction::VolumeUp: return R"(["add","volume",2])";
    case PlaybackAction::VolumeDown: return R"(["add","volume",-2])";
    case PlaybackAction::Previous: return R"(["playlist-prev","weak"])";
    case PlaybackAction::Next: return R"(["playlist-next","weak"])";
    }
    return {};
}
void ControllerInput::reset() { *this = ControllerInput{}; }
void ControllerInput::expire(Clock::time_point now)
{
    if (haveSequence_ && now - lastReceived_ >= InputTimeout) {
        released_ = held_;
        held_ = pressed_ = suppressed_ = {};
        nextRepeat_ = {};
        haveSequence_ = false;
        recovering_ = true;
    }
}
bool ControllerInput::update(const ControllerPacket& p, Clock::time_point now,
                             std::vector<PlaybackAction>& actions)
{
    expire(now);
    if (haveSequence_) {
        const auto delta = std::uint32_t(p.frame - last_.frame);
        // Frame serial arithmetic handles rollover. Order resets at each blit;
        // upstream compares it numerically within a frame (not modulo 256).
        if ((delta == 0 && p.order <= last_.order) || delta >= 0x80000000u) return false;
    }
    const auto oldEffective = (held_[0] & ~suppressed_[0]) | (held_[1] & ~suppressed_[1]);
    for (unsigned c = 0; c < 2; ++c) {
        pressed_[c] = p.buttons[c] & ~held_[c];
        released_[c] = held_[c] & ~p.buttons[c];
        held_[c] = p.buttons[c];
        suppressed_[c] = recovering_ ? p.buttons[c] : suppressed_[c] & p.buttons[c];
    }
    const auto effective = (held_[0] & ~suppressed_[0]) | (held_[1] & ~suppressed_[1]);
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];
        if ((effective & b.mask) && !(oldEffective & b.mask)) {
            actions.push_back(b.action);
            nextRepeat_[i] = now + RepeatDelay;
        }
        if (!(effective & b.mask)) nextRepeat_[i] = {};
    }
    last_ = p;
    lastReceived_ = now;
    haveSequence_ = true;
    recovering_ = false;
    return true;
}
std::vector<PlaybackAction> ControllerInput::tick(Clock::time_point now)
{
    expire(now);
    std::vector<PlaybackAction> actions;
    const auto effective = (held_[0] & ~suppressed_[0]) | (held_[1] & ~suppressed_[1]);
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];
        if (b.repeat && (effective & b.mask) && now >= nextRepeat_[i]) {
            actions.push_back(b.action);
            nextRepeat_[i] = now + RepeatInterval; // Never catch up with a burst.
        }
    }
    return actions;
}
} // namespace mistercast
