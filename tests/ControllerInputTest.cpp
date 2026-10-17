#include "input/ControllerInput.h"
#include "input/ControllerReceiver.h"
#include "input/MpvIpc.h"
#include "stream/GroovyMister.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <unistd.h>

using namespace mistercast;
using namespace std::chrono_literals;
using namespace controller_button;
using A = PlaybackAction;
static_assert(GroovyMister::kDefaultPort == 32100);
static_assert(ControllerReceiver::Port == 32101);
namespace {
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
struct Fd {
    int value{-1};
    ~Fd() { if (value >= 0) ::close(value); }
    void close() { if (value >= 0) ::close(value); value = -1; }
};
bool readable(int fd, int timeout = 1000)
{
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, timeout) > 0;
}
std::string readLine(int fd)
{
    std::string result;
    const auto deadline = ControllerInput::Clock::now() + 2s;
    while (ControllerInput::Clock::now() < deadline) {
        if (!readable(fd, 20)) continue;
        char ch;
        if (::recv(fd, &ch, 1, 0) != 1) break;
        result += ch;
        if (ch == '\n') return result;
    }
    throw std::runtime_error("Timed out reading IPC line");
}
void writeText(int fd, const std::string& text)
{
    check(::send(fd, text.data(), text.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(text.size()), "mock IPC write");
}
std::vector<std::uint8_t> packet(std::uint32_t frame, std::uint8_t order,
                               std::uint16_t first, std::uint16_t second, std::size_t size = 9)
{
    // Mirrors groovy_send_joysticks' byte assignments, not native struct layout.
    std::vector<std::uint8_t> bytes(size, 0xab);
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (frame >> (8 * i)) & 255;
    bytes[4] = order;
    bytes[5] = first & 255; bytes[6] = first >> 8;
    bytes[7] = second & 255; bytes[8] = second >> 8;
    return bytes;
}
void decodeAndState()
{
    constexpr std::array<std::string_view, 7> commands{
        R"(["cycle","pause"])", R"(["seek",-5,"relative"])", R"(["seek",5,"relative"])",
        R"(["add","volume",2])", R"(["add","volume",-2])",
        R"(["playlist-prev","weak"])", R"(["playlist-next","weak"])"};
    for (unsigned i = 0; i < commands.size(); ++i)
        check(mpvCommand(static_cast<A>(i)) == commands[i], "mpv action JSON array");
    for (std::size_t size : {9, 17}) {
        auto p = decodeControllerPacket(packet(0x12345678, 0x9a, 0x1111, 0x2222, size));
        check(p && p->frame == 0x12345678 && p->order == 0x9a, "frame/order little endian");
        check(p->buttons == std::array<std::uint16_t, 2>{0x1111, 0x2222}, "both controller offsets");
    }
    for (unsigned size = 0; size <= 100; ++size) {
        if (size == 9 || size == 17) continue;
        check(!decodeControllerPacket(std::vector<std::uint8_t>(size)), "unsupported packet size accepted");
    }
    constexpr std::array<std::uint16_t, 7> masks{0x10, 0x2, 0x1, 0x8, 0x4, 0x400, 0x800};
    constexpr std::array expected{A::Pause, A::SeekBack, A::SeekForward, A::VolumeUp, A::VolumeDown, A::Previous, A::Next};
    const auto t = ControllerInput::Clock::now();
    for (unsigned controller = 0; controller < 2; ++controller) {
        for (std::size_t i = 0; i < masks.size(); ++i) {
            ControllerInput input;
            auto p = *decodeControllerPacket(packet(1, 1, controller == 0 ? masks[i] : 0,
                                                   controller == 1 ? masks[i] : 0));
            std::vector<A> actions;
            check(input.update(p, t, actions) && actions == std::vector{expected[i]}, "button mapping");
            check(input.pressed()[controller] == masks[i] && input.held()[controller] == masks[i], "pressed/held");
            actions.clear();
            check(!input.update(p, t + 1ms, actions) && actions.empty(), "duplicate packet");
            p.order++;
            check(input.update(p, t + 2ms, actions) && actions.empty(), "unchanged state retriggered");
            check(input.pressed()[controller] == 0 && input.released()[controller] == 0, "held transitions");
            check(input.tick(t + 399ms).empty(), "repeat too soon");
            const bool repeats = i >= 1 && i <= 4;
            check(input.tick(t + 400ms) == (repeats ? std::vector{expected[i]} : std::vector<A>{}), "first repeat / single press");
            check(input.tick(t + 549ms).empty(), "repeat rate");
            check(input.tick(t + 550ms) == (repeats ? std::vector{expected[i]} : std::vector<A>{}), "repeat interval");
            p.order++; p.buttons = {};
            input.update(p, t + 551ms, actions);
            check(input.released()[controller] == masks[i] && input.held()[controller] == 0, "release transition");
            check(input.tick(t + 900ms).empty(), "repeat after release");
        }
    }
    ControllerInput input;
    std::vector<A> actions;
    input.update({100, 2, {Button1 | Right, Button1 | Up}}, t, actions);
    check(actions == std::vector{A::Pause, A::SeekForward, A::VolumeUp}, "simultaneous actions/coalescing");
    actions.clear();
    check(!input.update({99, 255, {}}, t + 10ms, actions), "old frame");
    check(!input.update({100, 1, {}}, t + 10ms, actions), "old order");
    check(!input.update({100 + 0x80000000u, 3, {}}, t + 10ms, actions), "ambiguous half-range frame");
    input.update({100, 3, {0, Button1 | Up}}, t + 20ms, actions);
    check(input.held()[0] == 0 && input.held()[1] == (Button1 | Up), "independent state");
    check(actions.empty(), "overlapping pause retrigger");
    check(input.tick(t + 400ms) == std::vector{A::VolumeUp}, "release one controller leaves other held");
    check(input.tick(t + 10s) == std::vector{A::VolumeUp}, "catch-up burst or premature idle timeout");
    check(input.tick(t + 61s).empty() && input.held() == std::array<std::uint16_t, 2>{}, "timeout releases");
    input.update({1, 1, {Button1 | Right, 0}}, t + 62s, actions);
    check(actions.empty() && input.tick(t + 63s).empty(), "reconnection must release held buttons");
    input.update({1, 2, {}}, t + 63s, actions);
    input.update({1, 3, {Button1, 0}}, t + 64s, actions);
    check(actions == std::vector{A::Pause}, "reconnection rearm");
    input.reset(); actions.clear();
    input.update({0xffffffff, 255, {Right, 0}}, t, actions);
    actions.clear();
    check(input.update({0, 0, {}}, t + 1ms, actions), "frame rollover");
    input.update({0, 1, {0x3e0, 0xf000}}, t + 2ms, actions);
    check(actions.empty(), "unbound inputs generate actions");
    input.reset();
    input.update({0, 0, {Button1, 0}}, t, actions);
    check(actions == std::vector{A::Pause}, "session reset accepts initial sequence");
    input.reset(); actions.clear();
    input.update({1, 1, {Right, 0}}, t, actions);
    input.suppressHeld();
    check(input.tick(t + 500ms).empty(), "pending IPC must not replay held input");
    actions.clear();
    input.update({1, 2, {Right, 0}}, t + 501ms, actions);
    check(actions.empty(), "suppression must survive unchanged state");
    input.update({1, 3, {}}, t + 502ms, actions);
    input.update({1, 4, {Right, 0}}, t + 503ms, actions);
    check(actions == std::vector{A::SeekForward}, "release must rearm suppressed input");
}
struct Server {
    std::string directory, path;
    Fd fd;
    Server() {
        char temp[] = "/tmp/mistercast-controller-XXXXXX";
        auto* name = ::mkdtemp(temp);
        check(name != nullptr, "mkdtemp");
        directory = name; path = directory + "/mpv.sock";
        fd.value = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd.value < 0) throw std::runtime_error("Mock Unix socket: " + std::string(std::strerror(errno)));
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        path.copy(address.sun_path, sizeof(address.sun_path) - 1);
        check(::bind(fd.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "mock bind");
        check(::listen(fd.value, 8) == 0, "mock listen");
    }
    ~Server() { ::unlink(path.c_str()); ::rmdir(directory.c_str()); }
    int accept() {
        check(readable(fd.value), "IPC connection missing");
        const int result = ::accept4(fd.value, nullptr, nullptr, SOCK_CLOEXEC);
        check(result >= 0, "mock accept");
        return result;
    }
};
void ipcTests()
{
    Server server;
    MpvIpc ipc;
    std::string error;
    const auto t = ControllerInput::Clock::now();
    check(!ipc.connect("relative", t, error), "relative path accepted");
    check(!ipc.connect("/does/not/exist", t, error), "missing socket accepted");
    check(ipc.connect(server.path, t, error), "IPC connect");
    Fd client{server.accept()};
    check(ipc.service(t, error) && ipc.ready(), "IPC ready");
    constexpr std::array<std::string_view, 7> commands{
        R"(["cycle","pause"])", R"(["seek",-5,"relative"])", R"(["seek",5,"relative"])",
        R"(["add","volume",2])", R"(["add","volume",-2])",
        R"(["playlist-prev","weak"])", R"(["playlist-next","weak"])"};
    for (unsigned i = 0; i < commands.size(); ++i) {
        check(ipc.send(static_cast<A>(i), t, error), "IPC send");
        const auto expected = "{\"command\":" + std::string(commands[i]) + ",\"request_id\":" + std::to_string(i + 1) + "}\n";
        check(readLine(client.value) == expected, "JSON command format");
        writeText(client.value, "{\"request_id\":" + std::to_string(i + 1));
        check(ipc.service(t + 1ms, error), "fragmented reply part one");
        writeText(client.value, ",\"error\":\"success\"}\n");
        check(ipc.service(t + 2ms, error), "fragmented reply part two");
    }
    check(ipc.service(t + 2s, error), "acknowledged requests timed out");
    check(ipc.send(A::Pause, t + 2s, error), "send before exit");
    client.close();
    check(!ipc.service(t + 2s, error) && !ipc.ready(), "mpv exit undetected");
    check(ipc.send(A::Pause, t + 2s, error), "disconnected dispatch should drop without queue");
    check(ipc.connect(server.path, t, error), "reconnect");
    Fd next{server.accept()};
    check(ipc.service(t, error), "reconnect service");
    for (unsigned i = 0; i < MpvIpc::MaxInFlight; ++i)
        check(ipc.send(A::VolumeUp, t, error), "bounded sends");
    check(!ipc.send(A::VolumeUp, t, error) && !ipc.ready(), "unbounded outstanding commands");
    next.close();
    check(ipc.connect(server.path, t, error), "connect for timeout");
    Fd silent{server.accept()};
    check(ipc.service(t, error), "timeout service");
    check(ipc.send(A::Pause, t, error), "timeout send");
    const auto command = readLine(silent.value);
    const auto idPos = command.find("\"request_id\":") + 13;
    const auto id = command.substr(idPos, command.find('}', idPos) - idPos);
    // Unsolicited/nested IDs must not grant send credit or cancel the watchdog.
    writeText(silent.value, "{\"data\":{\"request_id\":" + id + "},\"event\":\"test\"}\n");
    check(ipc.service(t + 100ms, error), "event service");
    check(!ipc.service(t + 1s, error) && !ipc.ready(), "reply timeout not enforced");
}
struct UdpServer {
    Fd fd;
    sockaddr_in client{};
    std::uint16_t port{};
    UdpServer() {
        fd.value = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd.value < 0) throw std::runtime_error("Mock UDP socket: " + std::string(std::strerror(errno)));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(::bind(fd.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "UDP bind");
        socklen_t length = sizeof(address);
        check(::getsockname(fd.value, reinterpret_cast<sockaddr*>(&address), &length) == 0, "UDP address");
        port = ntohs(address.sin_port);
    }
    void registration() {
        check(readable(fd.value), "registration missing");
        std::array<char, 8> bytes;
        socklen_t length = sizeof(client);
        check(::recvfrom(fd.value, bytes.data(), bytes.size(), 0,
                         reinterpret_cast<sockaddr*>(&client), &length) == 1, "registration must be one byte");
    }
    void send(const std::vector<std::uint8_t>& bytes) {
        check(::sendto(fd.value, bytes.data(), bytes.size(), 0,
                       reinterpret_cast<sockaddr*>(&client), sizeof(client)) == static_cast<ssize_t>(bytes.size()), "UDP packet send");
    }
};
void receiverTests()
{
    check(ControllerReceiver::Port == 32101, "controller server port");
    Server mpv;
    UdpServer mister;
    std::mutex mutex;
    std::vector<std::string> warnings;
    auto warn = [&](const std::string& message) { std::lock_guard lock(mutex); warnings.push_back(message); };
    ControllerReceiver receiver;
    std::string error;
    check(!receiver.start("127.0.0.1", "", warn, error, mister.port), "empty IPC path accepted");
    for (unsigned session = 0; session < 3; ++session) {
        check(receiver.start("127.0.0.1", mpv.path, warn, error, mister.port), "receiver startup");
        mister.registration();
        Fd client{mpv.accept()};
        mister.send(packet(1, 1, Button1, 0));
        check(!readable(client.value, 50), "commands before streaming activation");
        receiver.activate();
        // Neutral state releases the pre-activation press without firing it.
        mister.send(packet(2, 1, 0, 0));
        mister.send(packet(3, 1, 0, Button1, 17));
        auto command = readLine(client.value);
        check(command.find(std::string(mpvCommand(A::Pause))) != std::string::npos, "controller 2 IPC action");
        writeText(client.value, "{\"request_id\":1,\"error\":\"success\"}\n");
        mister.send(packet(3, 1, 0, Button1));
        mister.send(packet(4, 1, 0, Button1));
        mister.send(packet(2, 200, LeftShoulder, 0));
        mister.send(packet(5, 1, LeftShoulder, 0, 18));
        check(!readable(client.value, 50), "duplicate, stale or malformed action");
        UdpServer impostor;
        impostor.client = mister.client;
        impostor.send(packet(1000, 1, LeftShoulder, 0));
        check(!readable(client.value, 50), "unexpected sender accepted");
        mister.send(packet(5, 1, 0, 0));
        mister.send(packet(6, 1, Right, 0));
        command = readLine(client.value);
        check(command.find(std::string(mpvCommand(A::SeekForward))) != std::string::npos, "seek press");
        writeText(client.value, "{\"request_id\":2,\"error\":\"success\"}\n");
        command = readLine(client.value);
        check(command.find(std::string(mpvCommand(A::SeekForward))) != std::string::npos, "seek hold");
        writeText(client.value, "{\"request_id\":3,\"error\":\"success\"}\n");
        mister.send(packet(7, 1, 0, 0));
        check(!readable(client.value, 180), "seek continues after release");
        if (session == 0) {
            client.close();
            // Socket EOF terminates controls; no connection to a replacement process.
            check(!readable(mpv.fd.value, 100), "automatic reconnect after mpv exit");
        }
        const auto before = ControllerInput::Clock::now();
        receiver.stop();
        receiver.stop();
        check(ControllerInput::Clock::now() - before < 500ms, "shutdown blocked");
    }
    // Startup without mpv is retried without accumulating commands or warnings.
    {
        std::lock_guard lock(mutex);
        warnings.clear();
    }
    check(receiver.start("127.0.0.1", mpv.directory + "/missing.sock", warn, error, mister.port), "missing mpv receiver startup");
    mister.registration();
    receiver.activate();
    mister.send(packet(1, 1, Right, 0));
    check(!readable(mpv.fd.value, 100), "connected to wrong socket");
    receiver.stop();
    std::lock_guard lock(mutex);
    check(warnings.size() == 1, "missing socket warning count");
}
} // namespace
int main(int argc, char** argv)
{
    try {
        const std::string mode = argc > 1 ? argv[1] : "all";
        check(mode == "all" || mode == "unit" || mode == "ipc" || mode == "receiver", "unknown test mode");
        if (mode == "all" || mode == "unit") {
            decodeAndState(); std::cout << "PASS decoding, mapping, transitions, repeats, ordering, resets\n";
        }
        if (mode == "all" || mode == "ipc") {
            ipcTests(); std::cout << "PASS JSON IPC, fragmented replies, exit, queue bound, timeout\n";
        }
        if (mode == "all" || mode == "receiver") {
            receiverTests(); std::cout << "PASS UDP registration, sender filtering, dispatch, lifecycle\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
