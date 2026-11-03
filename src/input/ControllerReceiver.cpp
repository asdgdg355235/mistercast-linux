#include "input/ControllerReceiver.h"
#include "input/ControllerInput.h"
#include "input/MpvIpc.h"
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
                               Warning report, std::string& error, std::uint16_t port)
{
    stop();
    error.clear();
    if (path.empty() || path.front() != '/' || path.size() >= 108) {
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
    // BindInputs sends exactly once before CMD_INIT. Standard UDP setInit
    // consumes one queued byte; sending periodic registrations would leave
    // stale endpoints queued for later sessions. There is no registration ACK.
    const unsigned char registration = 0;
    if (::send(udp.fd, &registration, 1, MSG_NOSIGNAL) != 1) {
        error = "UDP registration failed: " + std::string(std::strerror(errno));
        return false;
    }
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (::getsockname(udp.fd, reinterpret_cast<sockaddr*>(&local), &length) < 0) {
        error = "UDP endpoint lookup failed: " + std::string(std::strerror(errno));
        return false;
    }
    const auto endpoint = std::to_string(ntohs(local.sin_port)) + " peer " + host + ":" + std::to_string(port);
    try {
        thread_ = std::jthread([this, fd = udp.fd, path, endpoint, report = std::move(report)](std::stop_token stop) {
            Socket socket{fd};
            ControllerInput input;
            MpvIpc ipc;
            bool opened = false, ready = false;
            auto retry = Clock::time_point{};
            auto lastValid = Clock::now();
            auto nextSummary = lastValid + ControllerInput::InputTimeout;
            auto nextError = Clock::time_point{};
            std::uint64_t raw = 0, valid = 0, stale = 0, malformed = 0, other = 0, transitions = 0, commands = 0;
            auto notify = [&](const std::string& text) { if (report) report(text + "; UDP local port " + endpoint); };
            auto ipcFailure = [&](const std::string& text, Clock::time_point now) {
                ipc.close(); opened = ready = false; input.suppressHeld();
                retry = now + std::chrono::seconds(1);
                if (now >= nextError) { notify(text); nextError = now + std::chrono::seconds(5); }
            };
            while (!stop.stop_requested()) {
                const auto now = Clock::now();
                std::string message;
                if (!opened && now >= retry) {
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
                    const auto n = ::recv(socket.fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_TRUNC);
                    if (n < 0) {
                        const int e = errno;
                        if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR) break;
                        if (now >= nextError) { notify("UDP socket receive failed: " + std::string(std::strerror(e))); nextError = now + std::chrono::seconds(5); }
                        if (e == EBADF || e == ENOTSOCK) return;
                        break;
                    }
                    ++raw;
                    if (n == 37 || n == 41) { ++other; continue; }
                    if (n != 9 && n != 17) {
                        if (++malformed == 1) notify("Malformed controller datagram: length=" + std::to_string(n));
                        continue;
                    }
                    const auto packet = *decodeControllerPacket(std::span(bytes).first(static_cast<std::size_t>(n)));
                    std::vector<PlaybackAction> actions;
                    if (!input.update(packet, now, actions)) { ++stale; continue; }
                    ++valid;
                    lastValid = now;
                    if (valid == 1) notify("Valid controller snapshot received");
                    if (input.pressed()[0] || input.pressed()[1] || input.released()[0] || input.released()[1]) ++transitions;
                    dispatch(actions);
                }
                dispatch(input.tick(now));
                if (now >= nextSummary) {
                    notify(std::string(raw == 0 ? "No controller datagrams received" : "Controller reception summary") +
                        ": datagrams=" + std::to_string(raw) + " valid=" + std::to_string(valid) +
                        " stale=" + std::to_string(stale) + " malformed=" + std::to_string(malformed) +
                        " ps2=" + std::to_string(other) + " transitions=" + std::to_string(transitions) +
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
        });
        udp.fd = -1;
    } catch (const std::exception& exception) {
        error = "Cannot start controller worker: " + std::string(exception.what());
        return false;
    }
    return true;
}
} // namespace mistercast
