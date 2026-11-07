#include "input/VirtualGamepad.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace mistercast {
namespace {
// Preserve Groovy RetroArch B1-B10; the updated fork uses bits 14/15 for L3/R3.
constexpr std::array keys{BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,
    BTN_SELECT, BTN_START, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
    BTN_THUMBL, BTN_THUMBR};
constexpr std::array axes{ABS_X, ABS_Y, ABS_RX, ABS_RY};
int hatX(std::uint16_t b) { return !!(b & 1) - !!(b & 2); }
int hatY(std::uint16_t b) { return !!(b & 4) - !!(b & 8); }
input_event event(unsigned short type, unsigned short code, int value)
{
    input_event e{}; e.type = type; e.code = code; e.value = value; return e;
}
}
GamepadState gamepadState(const ControllerPacket& p, unsigned slot)
{
    GamepadState s{p.buttons.at(slot), p.axes.at(slot)};
    if (!p.analog) s.axes = {hatX(s.buttons), hatY(s.buttons), 0, 0};
    return s;
}
std::vector<input_event> gamepadEvents(const GamepadState& old, const GamepadState& s, bool analog)
{
    std::vector<input_event> out;
    for (unsigned i = 0; i < keys.size(); ++i) {
        const auto mask = 1u << (i + 4);
        if ((old.buttons ^ s.buttons) & mask) out.push_back(event(EV_KEY, keys[i], !!(s.buttons & mask)));
    }
    if (hatX(old.buttons) != hatX(s.buttons)) out.push_back(event(EV_ABS, ABS_HAT0X, hatX(s.buttons)));
    if (hatY(old.buttons) != hatY(s.buttons)) out.push_back(event(EV_ABS, ABS_HAT0Y, hatY(s.buttons)));
    for (unsigned a = 0; a < (analog ? 4u : 2u); ++a)
        if (old.axes[a] != s.axes[a]) out.push_back(event(EV_ABS, axes[a], s.axes[a]));
    if (!out.empty()) out.push_back(event(EV_SYN, SYN_REPORT, 0));
    return out;
}
VirtualGamepad::~VirtualGamepad() { close(); }
bool VirtualGamepad::create(unsigned slot, bool analog, const std::string& endpoint, std::string& error)
{
    close();
    analog_ = analog;
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    auto fail = [&] {
        const int e = errno;
        close();
        error = "Virtual gamepad unavailable: /dev/uinput: " + std::string(std::strerror(e)) +
            "; load uinput and install packaging/70-mistercast-input.rules (do not run the app as root)";
        return false;
    };
    if (fd_ < 0) return fail();
    if (::ioctl(fd_, UI_SET_EVBIT, EV_SYN) < 0 || ::ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0 || ::ioctl(fd_, UI_SET_EVBIT, EV_ABS) < 0) return fail();
    for (auto key : keys) if (::ioctl(fd_, UI_SET_KEYBIT, key) < 0) return fail();
    auto enableAxis = [&](int code, int min, int max) {
        uinput_abs_setup a{};
        a.code = code; a.absinfo.minimum = min; a.absinfo.maximum = max;
        // MiSTer already applies the configured stick dead zone. No rescaling,
        // extra filtering, or invented 16-bit precision on the Linux side.
        return ::ioctl(fd_, UI_SET_ABSBIT, code) == 0 && ::ioctl(fd_, UI_ABS_SETUP, &a) == 0;
    };
    if (!enableAxis(ABS_HAT0X, -1, 1) || !enableAxis(ABS_HAT0Y, -1, 1)) return fail();
    for (unsigned a = 0; a < (analog ? 4u : 2u); ++a)
        if (!enableAxis(axes[a], analog ? -128 : -1, analog ? 127 : 1)) return fail();
    const auto phys = "mistercast/" + endpoint + "/slot" + std::to_string(slot + 1);
    if (::ioctl(fd_, UI_SET_PHYS, phys.c_str()) < 0) return fail();
    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL; // No borrowed USB VID/PID.
    setup.id.version = 1;
    std::snprintf(setup.name, sizeof(setup.name), "MiSTerCast Gamepad %u", slot + 1);
    if (::ioctl(fd_, UI_DEV_SETUP, &setup) < 0 || ::ioctl(fd_, UI_DEV_CREATE) < 0) return fail();
    std::array<char, 128> name{};
    if (::ioctl(fd_, UI_GET_SYSNAME(name.size()), name.data()) == 0) sysname_ = name.data();
    return true;
}
bool VirtualGamepad::update(const GamepadState& s, std::string& error)
{
    if (fd_ < 0) { error = "Valid input but no virtual gamepad exists"; return false; }
    const auto events = gamepadEvents(state_, s, analog_);
    if (events.empty()) return true;
    const auto size = events.size() * sizeof(input_event);
    ssize_t n;
    do { n = ::write(fd_, events.data(), size); } while (n < 0 && errno == EINTR);
    if (n != static_cast<ssize_t>(size)) {
        error = "Virtual gamepad event write failed: " + std::string(n < 0 ? std::strerror(errno) : "short write");
        close(); // Destroy on delivery failure; never leave a stale pressed device.
        return false;
    }
    state_ = s;
    return true;
}
void VirtualGamepad::close()
{
    if (fd_ >= 0) {
        const auto release = gamepadEvents(state_, {}, analog_);
        if (!release.empty()) ::write(fd_, release.data(), release.size() * sizeof(input_event));
        ::ioctl(fd_, UI_DEV_DESTROY);
        ::close(fd_);
    }
    fd_ = -1; state_ = {}; sysname_.clear();
}
bool GamepadInput::update(const ControllerPacket& p, Clock::time_point now)
{
    expire(now);
    if (live_) {
        const auto delta = std::uint32_t(p.frame - last_.frame);
        if ((delta == 0 && p.order <= last_.order) || delta >= 0x80000000u) return false;
    }
    last_ = p; received_ = now; live_ = true; return true;
}
bool GamepadInput::expire(Clock::time_point now)
{
    if (!live_ || now - received_ < timeout_) return false;
    live_ = false; last_ = {}; return true;
}
std::chrono::milliseconds controllerTimeout(double hz)
{
    if (!std::isfinite(hz) || hz <= 0) hz = 1;
    return std::chrono::milliseconds(static_cast<long long>(std::ceil(std::max(60.0, 2700.0 / hz + 15.0) * 1000)));
}
} // namespace mistercast
