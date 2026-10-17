#include "input/ControllerReceiver.h"
#include "input/ControllerInput.h"
#include "input/MpvIpc.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mistercast {
namespace {
struct Socket {
    int fd{-1};
    ~Socket() { if (fd >= 0) ::close(fd); }
};
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
bool ControllerReceiver::start(const std::string& host, const std::string& ipcPath,
                               Warning warning, std::string& error, std::uint16_t port)
{
    stop();
    if (ipcPath.empty() || ipcPath[0] != '/' || ipcPath.size() >= 108) {
        error = "Set an absolute mpv IPC socket path shorter than 108 bytes";
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const auto rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (rc != 0) { error = "Cannot resolve controller endpoint: " + std::string(gai_strerror(rc)); return false; }
    Socket socket;
    for (auto* address = addresses; address; address = address->ai_next) {
        socket.fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
        if (socket.fd < 0) continue;
        // Connected UDP filters both source IP and source port. Like BindInputs,
        // use an ephemeral client port; 32101 is the MiSTer server port.
        if (::connect(socket.fd, address->ai_addr, address->ai_addrlen) == 0) break;
        ::close(socket.fd);
        socket.fd = -1;
    }
    freeaddrinfo(addresses);
    if (socket.fd < 0) { error = "Cannot open controller UDP socket"; return false; }
    const std::uint8_t registration = 0; // Payload ignored by upstream.
    if (::send(socket.fd, &registration, 1, MSG_DONTWAIT | MSG_NOSIGNAL) != 1) {
        error = "Cannot register controller receiver: " + std::string(std::strerror(errno));
        return false;
    }
    try {
        thread_ = std::jthread([this, fd = socket.fd, ipcPath, warning = std::move(warning)](std::stop_token stop) {
            Socket udp{fd};
            ControllerInput input;
            MpvIpc ipc;
            bool connected = false, opened = false, warned = false, wasReady = false;
            auto retryAt = ControllerInput::Clock::time_point{};
            auto inactiveSince = ControllerInput::Clock::now();
            bool received = false, inputWarning = false;
            while (!stop.stop_requested()) {
                const auto now = ControllerInput::Clock::now();
                std::string message;
                if (!opened && now >= retryAt) {
                    opened = ipc.connect(ipcPath, now, message);
                    retryAt = now + std::chrono::seconds(1);
                    if (!opened && !warned) { warning(message); warned = true; }
                }
                if (opened && !ipc.service(now, message)) {
                    if (connected) { warning(message); break; }
                    opened = false;
                    if (!warned) { warning(message); warned = true; }
                }
                if (ipc.ready() && !wasReady) input.suppressHeld();
                wasReady = ipc.ready();
                connected = connected || ipc.ready();
                auto dispatch = [&](const std::vector<PlaybackAction>& actions) {
                    if (!active_ || !ipc.ready()) return true;
                    for (auto action : actions) {
                        if (stop.stop_requested() || !active_) return true;
                        if (!ipc.send(action, now, message)) { warning(message); return false; }
                    }
                    return true;
                };
                bool healthy = true;
                // Cap each drain so a UDP flood cannot starve shutdown or IPC reads.
                for (unsigned packets = 0; packets < 64 && !stop.stop_requested(); ++packets) {
                    std::array<std::uint8_t, 18> bytes{};
                    const auto n = ::recv(udp.fd, bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_TRUNC);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                        warning("Controller UDP receive failed: " + std::string(std::strerror(errno)));
                        healthy = false;
                        break;
                    }
                    if (n != 9 && n != 17) continue;
                    const auto packet = decodeControllerPacket(std::span(bytes).first(static_cast<std::size_t>(n)));
                    std::vector<PlaybackAction> actions;
                    if (packet && input.update(*packet, now, actions)) {
                        received = true;
                        inactiveSince = now;
                    }
                    if (!active_ || !ipc.ready()) input.suppressHeld();
                    if (!dispatch(actions)) { healthy = false; break; }
                }
                if (!healthy || !dispatch(input.tick(now))) break;
                if (!inputWarning && now - inactiveSince >= ControllerInput::InputTimeout) {
                    warning(received ? "Controller input timed out; release held buttons before resuming" :
                        "No controller packets received; enable Joysticks in Groovy_MiSTer OSD and restart streaming");
                    inputWarning = true;
                }
                pollfd p{udp.fd, POLLIN, 0};
                ::poll(&p, 1, 10);
            }
        });
        socket.fd = -1; // Worker owns the descriptor only after construction succeeds.
    } catch (const std::exception& exception) {
        error = "Cannot start controller worker: " + std::string(exception.what());
        return false;
    }
    return true;
}
} // namespace mistercast
