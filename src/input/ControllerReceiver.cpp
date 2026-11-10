#include "input/ControllerReceiver.h"
#include "input/ControllerInput.h"
#include "input/MpvIpc.h"
#include "input/VirtualGamepad.h"
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mistercast {
namespace {
struct Socket {
    int fd{-1};
    ~Socket() { if (fd >= 0) ::close(fd); }
};
using Clock = ControllerInput::Clock;
}
ControllerReceiver::~ControllerReceiver() { stop(); }
void ControllerReceiver::stop()
{
    active_ = false;
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}
bool ControllerReceiver::start(const std::string& host, const std::string& path,
                               Warning report, std::string& error, std::uint16_t port,
                               bool systemGamepads, double blitsPerSecond)
{
    stop();
    error.clear();
    if (!systemGamepads && (path.empty() || path.front() != '/' || path.size() >= 108)) {
        error = "mpv IPC requires an absolute socket path shorter than 108 bytes";
        return false;
    }
    addrinfo hints{}, *addresses = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    const int result = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses);
    if (result) { error = "UDP registration failed: " + std::string(gai_strerror(result)); return false; }
    Socket udp;
    int failure = 0;
    for (auto* a = addresses; a; a = a->ai_next) {
        udp.fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
        if (udp.fd >= 0 && ::connect(udp.fd, a->ai_addr, a->ai_addrlen) == 0) break;
        failure = errno;
        if (udp.fd >= 0) ::close(udp.fd);
        udp.fd = -1;
    }
    freeaddrinfo(addresses);
    if (udp.fd < 0) { error = "UDP socket setup failed: " + std::string(std::strerror(failure)); return false; }
    // flock ownership survives neither close nor process exit. Keep the regular
    // lock file in place: unlinking it would permit two owners on different inodes.
    sockaddr_in peer{};
    socklen_t peerLength = sizeof(peer);
    if (::getpeername(udp.fd, reinterpret_cast<sockaddr*>(&peer), &peerLength) < 0) {
        error = "Cannot inspect controller peer"; return false;
    }
    const auto lockName = "/tmp/mistercast-input-" + std::to_string(peer.sin_addr.s_addr) + "-" + std::to_string(port) + ".lock";
    Socket ownership{::open(lockName.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)};
    struct stat lockStat{};
    if (ownership.fd < 0 || ::fstat(ownership.fd, &lockStat) < 0) {
        error = "Cannot open controller ownership file: " + std::string(std::strerror(errno));
        return false;
    }
    if (!S_ISREG(lockStat.st_mode) || lockStat.st_uid != ::geteuid() || lockStat.st_nlink != 1) {
        error = "Unsafe controller ownership file: " + lockName;
        return false;
    }
    if (::flock(ownership.fd, LOCK_EX | LOCK_NB) < 0) {
        error = "Controller endpoint already owned by another receiver, or lock failed: " + std::string(std::strerror(errno));
        return false;
    }
    // Discover the routed local interface using connect(), then bind a receive
    // socket to that address. recvfrom lets diagnostics count wrong endpoints
    // rather than hiding them in the connected-UDP kernel filter.
    sockaddr_in route{};
    socklen_t routeLength = sizeof(route);
    if (::getsockname(udp.fd, reinterpret_cast<sockaddr*>(&route), &routeLength) < 0) {
        error = "Cannot inspect controller route"; return false;
    }
    ::close(udp.fd);
    udp.fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    route.sin_port = 0;
    if (udp.fd < 0 || ::bind(udp.fd, reinterpret_cast<sockaddr*>(&route), sizeof(route)) < 0) {
        error = "Cannot bind controller receive interface: " + std::string(std::strerror(errno)); return false;
    }
    // Register once before CMD_INIT. Legacy servers consume just the first
    // byte of this datagram. mpv keeps the original one-byte request.
    constexpr std::array<std::uint8_t, 1> legacyRegistration{0};
    const std::span<const std::uint8_t> registration = systemGamepads
        ? std::span<const std::uint8_t>(TriggerRegistration)
        : std::span<const std::uint8_t>(legacyRegistration);
    if (::sendto(udp.fd, registration.data(), registration.size(), MSG_NOSIGNAL,
                 reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) != static_cast<ssize_t>(registration.size())) {
        error = "UDP registration failed: " + std::string(std::strerror(errno));
        return false;
    }
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (::getsockname(udp.fd, reinterpret_cast<sockaddr*>(&local), &length) < 0) {
        error = "UDP endpoint lookup failed: " + std::string(std::strerror(errno));
        return false;
    }
    const auto endpoint = std::string(::inet_ntoa(local.sin_addr)) + ":" + std::to_string(ntohs(local.sin_port)) + " peer " + host + ":" + std::to_string(port);
    try {
        thread_ = std::jthread([this, fd = udp.fd, owner = ownership.fd, path, endpoint, systemGamepads, blitsPerSecond, peer, report = std::move(report)](std::stop_token stop) {
            Socket socket{fd};
            Socket ownerSocket{owner};
            ControllerInput input;
            GamepadInput gamepadInput(controllerTimeout(blitsPerSecond));
            std::array<VirtualGamepad, 2> pads;
            std::array<Clock::time_point, 2> deviceRetry{};
            MpvIpc ipc;
            bool opened = false, ready = false;
            auto retry = Clock::time_point{};
            auto lastValid = Clock::now();
            auto nextSummary = lastValid + ControllerInput::InputTimeout;
            auto nextError = Clock::time_point{};
            std::uint64_t raw = 0, valid = 0, stale = 0, malformed = 0, unexpected = 0, other = 0, transitions = 0, commands = 0;
            auto notify = [&](const std::string& text) { if (report) report(text + "; UDP local port " + endpoint); };
            auto ipcFailure = [&](const std::string& text, Clock::time_point now) {
                ipc.close(); opened = ready = false; input.suppressHeld();
                retry = now + std::chrono::seconds(1);
                if (now >= nextError) { notify(text); nextError = now + std::chrono::seconds(5); }
            };
            if (systemGamepads) notify("Controller registration sent; peer IP/port validated on receipt; receipt unconfirmed; watchdog_ms=" +
                std::to_string(controllerTimeout(blitsPerSecond).count()));
            auto resetPads = [&](const std::string& reason) {
                for (auto& pad : pads) pad.close();
                notify("Virtual gamepad slots 0/1 reset: " + reason);
            };
            auto syncPads = [&](Clock::time_point now) {
                if (!systemGamepads || !active_ || !gamepadInput.live()) return;
                const auto& packet = gamepadInput.packet();
                const auto selected = gamepadSlots_.load();
                for (unsigned slot = 0; slot < pads.size(); ++slot) {
                    auto& pad = pads[slot];
                    if ((selected & (1u << slot)) == 0) {
                        pad.close();
                        deviceRetry[slot] = {};
                        continue;
                    }
                    if (pad.exists() && pad.analog() != packet.analog) pad.close();
                    if (!pad.exists()) {
                        if (now < deviceRetry[slot]) continue;
                        std::string error;
                        if (!pad.create(slot, packet.analog, endpoint, error)) {
                            notify(error + "; valid input present; retry in 5 seconds");
                            deviceRetry[slot] = now + std::chrono::seconds(5);
                            continue;
                        }
                        notify("Created MiSTerCast Gamepad " + std::to_string(slot + 1) +
                            " slot=" + std::to_string(slot) + " sysfs=/sys/devices/virtual/input/" + pad.sysname() +
                            (packet.analog ? " analog 8-bit" : " digital"));
                    }
                    std::string error;
                    if (!pad.update(gamepadState(packet, slot), error)) {
                        notify(error); deviceRetry[slot] = now + std::chrono::seconds(5);
                    }
                }
            };
            while (!stop.stop_requested()) {
                const auto now = Clock::now();
                std::string message;
                if (systemGamepads && gamepadInput.expire(now))
                    resetPads("input watchdog expired; stream restart may be needed to register again");
                if (!systemGamepads && !opened && now >= retry) {
                    opened = ipc.connect(path, now, message);
                    if (!opened) ipcFailure("mpv IPC disconnected: " + message, now);
                }
                if (opened && !ipc.service(now, message)) ipcFailure("mpv IPC disconnected: " + message, now);
                if (ipc.ready() && !ready) input.suppressHeld();
                ready = ipc.ready();
                auto dispatch = [&](const std::vector<PlaybackAction>& actions) {
                    if (!active_ || !ready) { input.suppressHeld(); return; }
                    for (auto action : actions) {
                        if (stop.stop_requested() || !active_) return;
                        if (!ipc.send(action, now, message)) { ipcFailure("mpv command delivery failed: " + message, now); return; }
                        ++commands;
                    }
                };
                for (unsigned drain = 0; drain < 64 && !stop.stop_requested(); ++drain) {
                    std::array<std::uint8_t, 42> bytes{};
                    sockaddr_in sender{};
                    socklen_t senderLength = sizeof(sender);
                    const auto n = ::recvfrom(socket.fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_TRUNC,
                        reinterpret_cast<sockaddr*>(&sender), &senderLength);
                    if (n < 0) {
                        const int e = errno;
                        if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR) break;
                        if (now >= nextError) { notify("UDP socket receive failed: " + std::string(std::strerror(e))); nextError = now + std::chrono::seconds(5); }
                        if (e == EBADF || e == ENOTSOCK) return;
                        break;
                    }
                    ++raw;
                    if (sender.sin_addr.s_addr != peer.sin_addr.s_addr || sender.sin_port != peer.sin_port) {
                        if (++unexpected == 1) notify("Rejected traffic from unexpected endpoint " +
                            std::string(::inet_ntoa(sender.sin_addr)) + ":" + std::to_string(ntohs(sender.sin_port)));
                        continue;
                    }
                    if (n == 37 || n == 41) { ++other; continue; }
                    const auto packet = n <= static_cast<ssize_t>(bytes.size())
                        ? decodeControllerPacket(std::span(bytes).first(static_cast<std::size_t>(n)), systemGamepads)
                        : std::nullopt;
                    if (!packet) {
                        if (++malformed == 1) notify("Malformed controller datagram: length=" + std::to_string(n));
                        continue;
                    }
                    const auto oldPacket = gamepadInput.packet();
                    std::vector<PlaybackAction> actions;
                    if (systemGamepads ? !gamepadInput.update(*packet, now) : !input.update(*packet, now, actions)) { ++stale; continue; }
                    ++valid;
                    lastValid = now;
                    if (valid == 1) notify("Valid controller snapshot received");
                    if (systemGamepads && (oldPacket.buttons != packet->buttons ||
                        oldPacket.axes != packet->axes || oldPacket.triggers != packet->triggers)) ++transitions;
                    if (!systemGamepads && (input.pressed()[0] || input.pressed()[1] || input.released()[0] || input.released()[1])) ++transitions;
                    if (systemGamepads) syncPads(now);
                    else dispatch(actions);
                }
                if (systemGamepads) {
                    syncPads(now);
                } else dispatch(input.tick(now));
                if (now >= nextSummary) {
                    notify(std::string(raw == 0 ? "No controller datagrams received" : "Controller reception summary") +
                        ": datagrams=" + std::to_string(raw) + " valid=" + std::to_string(valid) +
                        " stale=" + std::to_string(stale) + " malformed=" + std::to_string(malformed) +
                        " unexpected=" + std::to_string(unexpected) + " ps2=" + std::to_string(other) + " transitions=" + std::to_string(transitions) +
                        " commands=" + std::to_string(commands) + " last_valid_age_ms=" +
                        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now-lastValid).count()) +
                        (raw == 0 ? "; registration receipt unconfirmed; check network filtering or restart stream to initialize registration" : ""));
                    nextSummary = now + ControllerInput::InputTimeout;
                }
                pollfd event{socket.fd, POLLIN, 0};
                if (::poll(&event, 1, 10) < 0 && errno != EINTR && now >= nextError) {
                    notify("UDP socket poll failed: " + std::string(std::strerror(errno)));
                    nextError = now + std::chrono::seconds(5);
                }
            }
            if (systemGamepads) resetPads("receiver shutdown");
        });
        udp.fd = -1;
        ownership.fd = -1;
    } catch (const std::exception& exception) {
        error = "Cannot start controller worker: " + std::string(exception.what());
        return false;
    }
    return true;
}
} // namespace mistercast
