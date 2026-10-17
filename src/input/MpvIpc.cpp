#include "input/MpvIpc.h"
#include <cerrno>
#include <charconv>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace mistercast {
namespace {
// Extract only a top-level numeric request_id. Ignore events, nested data and
// quoted/escaped occurrences. mpv is the local JSON producer; never eval replies.
std::optional<std::uint64_t> replyId(std::string_view line)
{
    int depth = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '{' || line[i] == '[') ++depth;
        else if (line[i] == '}' || line[i] == ']') --depth;
        else if (line[i] == '"') {
            const auto begin = ++i;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\') ++i;
                ++i;
            }
            if (i >= line.size()) return {};
            if (depth != 1 || line.substr(begin, i - begin) != "request_id") continue;
            auto pos = line.find_first_not_of(" \t\r", i + 1);
            if (pos == std::string_view::npos || line[pos] != ':') continue;
            pos = line.find_first_not_of(" \t\r", pos + 1);
            if (pos == std::string_view::npos) return {};
            std::uint64_t id{};
            auto result = std::from_chars(line.data() + pos, line.data() + line.size(), id);
            if (result.ec != std::errc{}) return {};
            pos = line.find_first_not_of(" \t\r", result.ptr - line.data());
            if (pos != std::string_view::npos && (line[pos] == ',' || line[pos] == '}')) return id;
        }
    }
    return {};
}
}
MpvIpc::~MpvIpc() { close(); }
void MpvIpc::close()
{
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    connecting_ = false;
    pending_.clear();
    incoming_.clear();
}
bool MpvIpc::connect(const std::string& path, Clock::time_point now, std::string& error)
{
    close();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path) || path[0] != '/') {
        error = "mpv IPC requires an absolute Unix socket path shorter than 108 bytes";
        return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) { error = "Cannot create mpv IPC socket: " + std::string(std::strerror(errno)); return false; }
    const int result = ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result < 0 && errno != EINPROGRESS) {
        error = "Cannot connect to mpv IPC: " + std::string(std::strerror(errno));
        close();
        return false;
    }
    connecting_ = result < 0;
    connectAt_ = now;
    return true;
}
bool MpvIpc::service(Clock::time_point now, std::string& error)
{
    if (fd_ < 0) return false;
    auto fail = [&](const char* message) { error = message; close(); return false; };
    if (connecting_) {
        pollfd p{fd_, POLLOUT, 0};
        if (::poll(&p, 1, 0) > 0) {
            int status{};
            socklen_t length = sizeof(status);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &status, &length) < 0 || status != 0)
                return fail("mpv IPC connection failed");
            connecting_ = false;
        } else if (now - connectAt_ >= ReplyTimeout) return fail("mpv IPC connection timed out");
        else return true;
    }
    // Bound work even if the peer floods events. Buffer storage is capped too.
    for (int reads = 0; reads < 8; ++reads) {
        char buffer[1024];
        const auto n = ::recv(fd_, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n == 0) return fail("mpv exited or closed its IPC connection; restart streaming to reconnect");
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            return fail("mpv IPC receive failed");
        }
        incoming_.append(buffer, static_cast<std::size_t>(n));
        if (incoming_.size() > 8192) return fail("mpv IPC reply exceeds 8192 bytes");
        std::size_t newline;
        while ((newline = incoming_.find('\n')) != std::string::npos) {
            if (const auto id = replyId(std::string_view(incoming_).substr(0, newline))) pending_.erase(*id);
            incoming_.erase(0, newline + 1);
        }
    }
    if (!pending_.empty() && now - pending_.begin()->second >= ReplyTimeout)
        return fail("mpv IPC reply timed out; controls stopped until streaming restarts");
    return true;
}
bool MpvIpc::send(PlaybackAction action, Clock::time_point now, std::string& error)
{
    if (!ready()) return true; // No queue while connecting.
    if (pending_.size() >= MaxInFlight) {
        error = "mpv IPC has 16 unanswered commands; controls stopped until streaming restarts";
        close();
        return false;
    }
    const auto id = nextId_++;
    const std::string line = "{\"command\":" + std::string(mpvCommand(action)) +
        ",\"request_id\":" + std::to_string(id) + "}\n";
    const auto n = ::send(fd_, line.data(), line.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n != static_cast<ssize_t>(line.size())) {
        // No retry of a partial command: reconnecting could duplicate an action.
        error = "mpv IPC write failed or stalled; controls stopped until streaming restarts";
        close();
        return false;
    }
    pending_.emplace(id, now);
    return true;
}
} // namespace mistercast
