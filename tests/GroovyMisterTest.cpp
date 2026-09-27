#include "core/Modeline.h"
#include "stream/GroovyMister.h"

#include <QtTest>
#include <lz4.h>
#include <cerrno>
#include <cstdarg>
#include <sys/ioctl.h>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace mistercast;

namespace {

ssize_t receivePacket(int socket, std::span<std::uint8_t> buffer, sockaddr_in& peer)
{
    pollfd descriptor{socket, POLLIN, 0};
    if (poll(&descriptor, 1, 1000) <= 0) {
        return -1;
    }
    socklen_t peerLength = sizeof(peer);
    return recvfrom(
        socket, buffer.data(), buffer.size(), 0,
        reinterpret_cast<sockaddr*>(&peer), &peerLength);
}

} // namespace

class GroovyMisterTest final : public QObject {
    Q_OBJECT

private slots:
    void lz4InterlacedProtocolAndAckCoherence();
    void deltaArithmeticAndSelection();
    void parityHistoryAndResync();
    void geometryAndUnsubmittedHistory();
    void deltaUdpRoundTrip_data();
    void deltaUdpRoundTrip();
    void failedSubmissionInvalidatesHistory();
};

void GroovyMisterTest::lz4InterlacedProtocolAndAckCoherence()
{
    const Modeline mode{
        "320x480i", 6.700, 320, 336, 367, 426, 480, 488, 493, 525, true};
    const int server = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    QVERIFY(server >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    QVERIFY(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    socklen_t addressLength = sizeof(address);
    QVERIFY(getsockname(server, reinterpret_cast<sockaddr*>(&address), &addressLength) == 0);
    const auto port = ntohs(address.sin_port);
    std::atomic_bool protocolValid{true};
    const auto check = [&protocolValid](bool condition) {
        if (!condition) {
            protocolValid.store(false);
        }
    };

    std::jthread mock([&] {
        std::array<std::uint8_t, 2048> packet{};
        sockaddr_in peer{};

        const auto initSize = receivePacket(server, packet, peer);
        const std::array<std::uint8_t, 5> expectedInit{2, 1, 3, 2, 0};
        check(initSize == static_cast<ssize_t>(expectedInit.size()) &&
            std::equal(expectedInit.begin(), expectedInit.end(), packet.begin()));

        std::array<std::uint8_t, 13> ack{};
        ack[12] = 0x47;
        check(sendto(
                  server, ack.data(), ack.size(), 0,
                  reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) ==
            static_cast<ssize_t>(ack.size()));

        const auto switchSize = receivePacket(server, packet, peer);
        const std::array<std::uint8_t, 26> expectedSwitch{
            0x03, 0xcd, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x1a, 0x40,
            0x40, 0x01, 0x50, 0x01, 0x6f, 0x01, 0xaa, 0x01,
            0xe0, 0x01, 0xe8, 0x01, 0xed, 0x01, 0x0d, 0x02, 0x01};
        check(switchSize == static_cast<ssize_t>(expectedSwitch.size()) &&
            std::equal(expectedSwitch.begin(), expectedSwitch.end(), packet.begin()));
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::memcpy(&width, packet.data() + 9, sizeof(width));
        std::memcpy(&height, packet.data() + 17, sizeof(height));
        check(width == mode.hActive && height == mode.vActive && packet[25] == 1);

        const auto blitSize = receivePacket(server, packet, peer);
        check(blitSize == 12 && packet[0] == 7 && packet[5] == 1);
        const std::array<std::uint8_t, 8> expectedFirstBlit{
            7, 1, 0, 0, 0, 1, 6, 1};
        check(std::equal(expectedFirstBlit.begin(), expectedFirstBlit.end(), packet.begin()));
        std::uint32_t compressedSize = 0;
        std::memcpy(&compressedSize, packet.data() + 8, sizeof(compressedSize));
        check(compressedSize > 0 && compressedSize < mode.payloadBytes());

        std::size_t payloadReceived = 0;
        while (payloadReceived < compressedSize) {
            const auto payloadSize = receivePacket(server, packet, peer);
            if (payloadSize <= 0) {
                protocolValid = false;
                break;
            }
            check(static_cast<std::size_t>(payloadSize) ==
                std::min<std::size_t>(1472, compressedSize - payloadReceived));
            payloadReceived += static_cast<std::size_t>(payloadSize);
        }
        check(payloadReceived == compressedSize);

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::array<std::uint8_t, 13> frameAck{};
        const std::uint32_t firstEcho = 1;
        const std::uint32_t firstDisplayed = 1;
        std::memcpy(frameAck.data(), &firstEcho, sizeof(firstEcho));
        std::memcpy(frameAck.data() + 6, &firstDisplayed, sizeof(firstDisplayed));
        frameAck[12] = 0xc7;
        check(sendto(
                  server, frameAck.data(), frameAck.size(), 0,
                  reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) ==
            static_cast<ssize_t>(frameAck.size()));

        const std::uint32_t duplicateDisplayed = 2;
        std::memcpy(frameAck.data() + 6, &duplicateDisplayed, sizeof(duplicateDisplayed));
        frameAck[12] = 0xe7;
        check(sendto(
                  server, frameAck.data(), frameAck.size(), 0,
                  reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) ==
            static_cast<ssize_t>(frameAck.size()));

        const auto audioCommandSize = receivePacket(server, packet, peer);
        const std::array<std::uint8_t, 3> expectedAudio{4, 8, 0};
        check(audioCommandSize == static_cast<ssize_t>(expectedAudio.size()) &&
            std::equal(expectedAudio.begin(), expectedAudio.end(), packet.begin()));
        std::uint16_t audioSize = 0;
        std::memcpy(&audioSize, packet.data() + 1, sizeof(audioSize));
        check(audioSize == 8);
        const auto audioPayloadSize = receivePacket(server, packet, peer);
        check(audioPayloadSize == audioSize);

        const auto incompressibleBlitSize = receivePacket(server, packet, peer);
        check(incompressibleBlitSize == 12 && packet[0] == 7 && packet[5] == 0);
        std::uint32_t randomCompressedSize = 0;
        std::memcpy(&randomCompressedSize, packet.data() + 8, sizeof(randomCompressedSize));
        check(randomCompressedSize >= mode.payloadBytes());
        payloadReceived = 0;
        while (payloadReceived < randomCompressedSize) {
            const auto payloadSize = receivePacket(server, packet, peer);
            if (payloadSize <= 0) {
                protocolValid = false;
                break;
            }
            check(static_cast<std::size_t>(payloadSize) ==
                std::min<std::size_t>(1472, randomCompressedSize - payloadReceived));
            payloadReceived += static_cast<std::size_t>(payloadSize);
        }
        check(payloadReceived == randomCompressedSize);

        frameAck.fill(0);
        const std::uint32_t secondEcho = 2;
        const std::uint32_t secondDisplayed = 2;
        std::memcpy(frameAck.data(), &secondEcho, sizeof(secondEcho));
        std::memcpy(frameAck.data() + 6, &secondDisplayed, sizeof(secondDisplayed));
        frameAck[12] = 0xef;
        check(sendto(
                  server, frameAck.data(), frameAck.size(), 0,
                  reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) ==
            static_cast<ssize_t>(frameAck.size()));
    });

    GroovyMister transport;
    std::string error;
    QVERIFY2(transport.connect("127.0.0.1", true, mode, error, port), error.c_str());
    const auto capacity = transport.prepareFrame();
    QVERIFY2(capacity.ready, capacity.warning.c_str());
    std::vector<std::uint8_t> frame(mode.payloadBytes());
    auto videoResult = transport.sendFrame(
        frame, 1, FieldParity::Field1EvenSourceLines);
    QVERIFY2(videoResult.status == VideoSubmitStatus::Sent, videoResult.error.c_str());
    transport.waitSync();
    QCOMPARE(transport.status().frameEcho, std::uint32_t{1});
    QCOMPARE(transport.status().frame, std::uint32_t{1});
    QVERIFY(!transport.status().vgaField);
    const std::array<std::int16_t, 4> audio{1, 2, 3, 4};
    QVERIFY2(transport.sendAudio(audio, error), error.c_str());
    QCOMPARE(transport.diagnostics().audioPacketsRequested, std::uint64_t{1});
    QCOMPARE(transport.diagnostics().audioPacketsSent, std::uint64_t{1});
    QCOMPARE(transport.diagnostics().audioPacketsCoreDisabled, std::uint64_t{0});

    std::uint32_t random = 0x12345678;
    for (auto& byte : frame) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<std::uint8_t>(random);
    }
    videoResult = transport.sendFrame(
        frame, 2, FieldParity::Field0OddSourceLines);
    QVERIFY2(videoResult.status == VideoSubmitStatus::Sent, videoResult.error.c_str());
    QVERIFY(videoResult.timings.transmittedBytes >= mode.payloadBytes());
    transport.waitSync();
    QCOMPARE(transport.status().frameEcho, std::uint32_t{2});
    QCOMPARE(transport.diagnostics().fpgaStatusSamples, std::uint64_t{2});
    QCOMPARE(transport.diagnostics().fpgaFrameskipSamples, std::uint64_t{1});
    QCOMPARE(transport.diagnostics().fpgaUnsyncedSamples, std::uint64_t{0});
    QCOMPARE(transport.diagnostics().fpgaQueueEmptySamples, std::uint64_t{0});
    QCOMPARE(transport.diagnostics().audioPacketsSent, std::uint64_t{1});
    const std::array<std::uint8_t, 1> invalidFrame{};
    videoResult = transport.sendFrame(
        invalidFrame, 3, FieldParity::Field0OddSourceLines);
    QCOMPARE(videoResult.status, VideoSubmitStatus::Fatal);
    QVERIFY(!videoResult.error.empty());
    transport.close();

    mock.join();
    close(server);
    QVERIFY(protocolValid.load());
}

namespace {

std::vector<std::uint8_t> randomField(std::size_t bytes, std::uint32_t seed)
{
    std::vector<std::uint8_t> raw(bytes);
    for (auto& byte : raw) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        byte = static_cast<std::uint8_t>(seed);
    }
    return raw;
}

std::vector<std::uint8_t> inflate(std::span<const std::uint8_t> payload, std::size_t bytes)
{
    std::vector<std::uint8_t> raw(bytes);
    const int count = LZ4_decompress_safe(
        reinterpret_cast<const char*>(payload.data()), reinterpret_cast<char*>(raw.data()),
        static_cast<int>(payload.size()), static_cast<int>(raw.size()));
    if (count != static_cast<int>(bytes)) {
        raw.clear();
    }
    return raw;
}

void reconstruct(std::vector<std::uint8_t>& delta, const std::vector<std::uint8_t>& previous)
{
    for (std::size_t i = 0; i < delta.size(); ++i) {
        delta[i] = static_cast<std::uint8_t>(delta[i] + previous[i]);
    }
}

const Modeline testMode{"64x64", 6.7, 64, 336, 367, 426, 64, 244, 247, 262, false};
constexpr auto field0 = FieldParity::Field0OddSourceLines;
constexpr auto field1 = FieldParity::Field1EvenSourceLines;

// Test-only linker wrappers forward real UDP calls except while a failure is
// armed. No production transport hooks or replacement packetizer are needed.
std::atomic_int payloadBudget{-1};
std::atomic_bool failCommand{false};
std::atomic_bool holdQueue{false};

} // namespace

extern "C" int __real_sendmmsg(int, mmsghdr*, unsigned int, int);
extern "C" ssize_t __real_send(int, const void*, std::size_t, int);
extern "C" int __real_ioctl(int, unsigned long, ...);

extern "C" int __wrap_sendmmsg(int fd, mmsghdr* messages, unsigned int count, int flags)
{
    const int budget = payloadBudget.load();
    if (budget == 0) {
        errno = EIO;
        return -1;
    }
    const int sent = __real_sendmmsg(fd, messages,
        budget < 0 ? count : std::min(count, static_cast<unsigned>(budget)), flags);
    if (budget > 0 && sent > 0) {
        payloadBudget -= sent;
    }
    return sent;
}

extern "C" ssize_t __wrap_send(int fd, const void* bytes, std::size_t size, int flags)
{
    if (failCommand.load()) {
        errno = EIO;
        return -1;
    }
    return __real_send(fd, bytes, size, flags);
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args;
    va_start(args, request);
    auto* pointer = va_arg(args, void*);
    va_end(args);
    if (request == TIOCOUTQ && holdQueue.load()) {
        *static_cast<int*>(pointer) = 32768;
        return 0;
    }
    return __real_ioctl(fd, request, pointer);
}

void GroovyMisterTest::deltaArithmeticAndSelection()
{
    GroovyVideoEncoder encoder;
    auto base = randomField(testMode.payloadBytes(), 0x12345678);
    base[0] = 250; base[1] = 5; base[2] = 255; base[3] = 0;
    auto choice = encoder.prepare(base, testMode, field0);
    QVERIFY(!choice.delta);
    QCOMPARE(inflate(choice.payload, base.size()), base);
    encoder.commit(base, field0, choice);

    auto current = base;
    current[0] = 5; current[1] = 250; current[2] = 0; current[3] = 255;
    choice = encoder.prepare(current, testMode, field0);
    QVERIFY(choice.delta);
    auto delta = inflate(choice.payload, current.size());
    QCOMPARE(delta.size(), current.size());
    QCOMPARE(delta[0], std::uint8_t{11});
    QCOMPARE(delta[1], std::uint8_t{245});
    QCOMPARE(delta[2], std::uint8_t{1});
    QCOMPARE(delta[3], std::uint8_t{255});
    reconstruct(delta, base);
    QCOMPARE(delta, current);
    encoder.commit(current, field0, choice);

    auto unrelated = randomField(base.size(), 0x87654321);
    choice = encoder.prepare(unrelated, testMode, field0);
    QVERIFY(!choice.delta);
    QCOMPARE(choice.rejection, GroovyVideoEncoder::Rejection::Content);
    QCOMPARE(inflate(choice.payload, unrelated.size()), unrelated);

    // Both candidates have the same zero prefix and independent random tails:
    // enough matches to try delta, but no 5% compressed-size improvement.
    std::fill(base.begin(), base.begin() + base.size() / 4, 0);
    std::fill(unrelated.begin(), unrelated.begin() + unrelated.size() / 4, 0);
    choice = encoder.prepare(base, testMode, field0);
    encoder.commit(base, field0, choice);
    choice = encoder.prepare(unrelated, testMode, field0);
    QVERIFY(!choice.delta);
    QCOMPARE(choice.rejection, GroovyVideoEncoder::Rejection::Size);
    QCOMPARE(inflate(choice.payload, unrelated.size()), unrelated);

    std::vector<std::uint8_t> zeros(base.size());
    choice = encoder.prepare(zeros, testMode, field0);
    encoder.commit(zeros, field0, choice);
    choice = encoder.prepare(zeros, testMode, field0);
    QVERIFY(!choice.delta);
    QCOMPARE(choice.rejection, GroovyVideoEncoder::Rejection::Content);
}

void GroovyMisterTest::parityHistoryAndResync()
{
    auto mode = testMode;
    mode.interlaced = true;
    mode.vActive *= 2;
    GroovyVideoEncoder encoder;
    const auto odd = randomField(mode.payloadBytes(), 0x12345678);
    const auto even = randomField(mode.payloadBytes(), 0x87654321);
    auto choice = encoder.prepare(odd, mode, field0);
    QVERIFY(!choice.delta);
    encoder.commit(odd, field0, choice);
    choice = encoder.prepare(even, mode, field1);
    QVERIFY(!choice.delta);
    encoder.commit(even, field1, choice);

    for (unsigned i = 1; i <= GroovyVideoEncoder::kFullRefreshInterval; ++i) {
        choice = encoder.prepare(odd, mode, field0);
        QCOMPARE(choice.forcedFull, i == GroovyVideoEncoder::kFullRefreshInterval);
        QCOMPARE(choice.delta, i < GroovyVideoEncoder::kFullRefreshInterval);
        auto decoded = inflate(choice.payload, odd.size());
        if (choice.delta) reconstruct(decoded, odd);
        QCOMPARE(decoded, odd);
        encoder.commit(odd, field0, choice);
    }
    choice = encoder.prepare(even, mode, field1);
    QVERIFY(choice.delta); // field 0's resync does not age field 1's history
    auto decoded = inflate(choice.payload, even.size());
    reconstruct(decoded, even);
    QCOMPARE(decoded, even);
    encoder.commit(even, field1, choice);
    choice = encoder.prepare(odd, mode, field0);
    QVERIFY(choice.delta); // full resync reset the counter

    // A natural full fallback also resets the counter.
    const auto fresh = randomField(odd.size(), 0x55667788);
    choice = encoder.prepare(fresh, mode, field0);
    QVERIFY(!choice.delta);
    encoder.commit(fresh, field0, choice);
    for (unsigned i = 1; i < GroovyVideoEncoder::kFullRefreshInterval; ++i) {
        choice = encoder.prepare(fresh, mode, field0);
        QVERIFY(choice.delta);
        encoder.commit(fresh, field0, choice);
    }
    encoder.invalidate();
    for (auto field : {field0, field1}) {
        choice = encoder.prepare(odd, mode, field);
        QVERIFY(!choice.delta);
        QVERIFY(!choice.forcedFull);
    }
}

void GroovyMisterTest::geometryAndUnsubmittedHistory()
{
    GroovyVideoEncoder encoder;
    auto mode = testMode;
    auto base = randomField(mode.payloadBytes(), 0x12345678);
    auto choice = encoder.prepare(base, mode, field0);
    // Preparing without successful submission must not create history.
    choice = encoder.prepare(base, mode, field0);
    QVERIFY(!choice.delta);
    encoder.commit(base, field0, choice);
    auto unsent = base;
    ++unsent[10];
    choice = encoder.prepare(unsent, mode, field0);
    QVERIFY(choice.delta);
    auto current = base;
    ++current[20];
    choice = encoder.prepare(current, mode, field0);
    auto decoded = inflate(choice.payload, base.size());
    reconstruct(decoded, base);
    QCOMPARE(decoded, current); // skipped candidate was not the base
    encoder.commit(current, field0, choice);

    // Same byte count with different geometry cannot reuse old history.
    mode.hActive *= 2;
    mode.vActive /= 2;
    choice = encoder.prepare(current, mode, field0);
    QVERIFY(!choice.delta);
    encoder.commit(current, field0, choice);
    mode.interlaced = true;
    mode.vActive *= 2;
    choice = encoder.prepare(current, mode, field0);
    QVERIFY(!choice.delta);
    encoder.commit(current, field0, choice);
    choice = encoder.prepare(current, mode, field1);
    QVERIFY(!choice.delta);
    encoder.commit(current, field1, choice);
    mode.interlaced = false;
    mode.vActive /= 2;
    choice = encoder.prepare(current, mode, field0);
    QVERIFY(!choice.delta); // progressive must establish its own base
    encoder.commit(current, field0, choice);
    choice = encoder.prepare(current, mode, field0);
    QVERIFY(choice.delta);
    choice = encoder.prepare(current, mode, field1);
    QVERIFY(choice.payload.empty());
    choice = encoder.prepare(current, mode, field0);
    QVERIFY(!choice.delta);
    encoder.commit(current, field0, choice);
    current.pop_back();
    QVERIFY(encoder.prepare(current, mode, field0).payload.empty());
    current.push_back(0);
    QVERIFY(!encoder.prepare(current, mode, field0).delta);
    mode.hActive /= 2;
    current.resize(mode.payloadBytes());
    QVERIFY(!encoder.prepare(current, mode, field0).delta);
}


namespace {

class LocalReceiver {
public:
    LocalReceiver()
    {
        socket_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
        address_.sin_family = AF_INET;
        address_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (socket_ < 0 || bind(socket_, reinterpret_cast<sockaddr*>(&address_), sizeof(address_)) < 0) {
            return;
        }
        socklen_t size = sizeof(address_);
        ready_ = getsockname(socket_, reinterpret_cast<sockaddr*>(&address_), &size) == 0;
    }
    ~LocalReceiver() { if (socket_ >= 0) ::close(socket_); }

    bool connectTransport(GroovyMister& transport, const Modeline& mode, std::string& error)
    {
        if (!ready_) return false;
        bool valid = false;
        std::jthread handshake([&] {
            std::array<std::uint8_t, 2048> bytes{};
            auto size = receive(bytes);
            // A normal close may precede a reconnect on this test socket.
            if (size == 1 && bytes[0] == 1) size = receive(bytes);
            if (size != 5 || bytes[0] != 2 || bytes[1] != 1) return;
            std::array<std::uint8_t, 13> ack{};
            ack[12] = 0xc7;
            valid = sendto(socket_, ack.data(), ack.size(), 0,
                reinterpret_cast<sockaddr*>(&peer_), sizeof(peer_)) == 13;
        });
        const bool connected = transport.connect("127.0.0.1", false, mode, error, ntohs(address_.sin_port));
        handshake.join();
        if (!connected || !valid) return false;
        std::array<std::uint8_t, 2048> bytes{};
        return receive(bytes) == 26 && bytes[0] == 3;
    }
    ssize_t receive(std::span<std::uint8_t> bytes)
    {
        return receivePacket(socket_, bytes, peer_);
    }
    bool field(const std::vector<std::uint8_t>& source, std::uint32_t number,
        FieldParity parity, bool delta, std::size_t transmitted,
        std::vector<std::uint8_t>& previous)
    {
        std::array<std::uint8_t, 2048> bytes{};
        const auto headerSize = receive(bytes);
        std::uint32_t frame = 0, size = 0;
        std::memcpy(&frame, bytes.data() + 1, sizeof(frame));
        std::memcpy(&size, bytes.data() + 8, sizeof(size));
        if (headerSize != (delta ? 13 : 12) || bytes[0] != 7 ||
            bytes[5] != static_cast<std::uint8_t>(parity) || frame != number ||
            size != transmitted || (delta && bytes[12] != 1)) return false;
        std::vector<std::uint8_t> payload;
        while (payload.size() < size) {
            const auto received = receive(bytes);
            if (received != static_cast<ssize_t>(std::min<std::size_t>(1472, size - payload.size()))) {
                return false;
            }
            payload.insert(payload.end(), bytes.begin(), bytes.begin() + received);
        }
        auto raw = inflate(payload, source.size());
        if (raw.size() != source.size()) return false;
        if (delta) {
            if (previous.size() != raw.size()) return false;
            reconstruct(raw, previous);
        }
        if (raw != source) return false;
        previous = std::move(raw);
        return true;
    }
private:
    int socket_{-1};
    bool ready_{};
    sockaddr_in address_{};
    sockaddr_in peer_{};
};

} // namespace

void GroovyMisterTest::deltaUdpRoundTrip_data()
{
    QTest::addColumn<bool>("interlaced");
    QTest::newRow("progressive") << false;
    QTest::newRow("interlaced") << true;
}

void GroovyMisterTest::deltaUdpRoundTrip()
{
    QFETCH(bool, interlaced);
    auto mode = testMode;
    mode.interlaced = interlaced;
    if (interlaced) mode.vActive *= 2;
    LocalReceiver receiver;
    GroovyMister transport;
    std::string error;
    QVERIFY2(receiver.connectTransport(transport, mode, error), error.c_str());
    std::array<std::vector<std::uint8_t>, 2> source{
        randomField(mode.payloadBytes(), 0x12345678), randomField(mode.payloadBytes(), 0x87654321)};
    std::array<std::vector<std::uint8_t>, 2> previous;
    const unsigned parities = interlaced ? 2 : 1;
    for (unsigned i = 0; i < 32 * parities; ++i) {
        const unsigned slot = i % parities;
        const auto parity = static_cast<FieldParity>(slot);
        ++source[slot][i];
        const auto result = transport.sendFrame(source[slot], i + 1, parity);
        QCOMPARE(result.status, VideoSubmitStatus::Sent);
        const bool expectedDelta = (i / parities) % GroovyVideoEncoder::kFullRefreshInterval != 0;
        QVERIFY(receiver.field(source[slot], i + 1, parity, expectedDelta,
            result.timings.transmittedBytes, previous[slot]));
    }
    QCOMPARE(transport.diagnostics().fullLz4Sends, std::uint64_t{2 * parities});
    QCOMPARE(transport.diagnostics().deltaLz4Sends, std::uint64_t{30 * parities});
    QCOMPARE(transport.diagnostics().forcedFullResyncs, std::uint64_t{parities});
    // No frame ACKs were sent: delta history is successful-send based.
    QCOMPARE(transport.status().frameEcho, std::uint32_t{0});

    const auto saved = source[0];
    ++source[0][100];
    holdQueue = true;
    const auto dropped = transport.sendFrame(source[0], 100, field0);
    holdQueue = false;
    QCOMPARE(dropped.status, VideoSubmitStatus::DroppedBeforeCommand);
    source[0] = saved;
    ++source[0][101];
    auto result = transport.sendFrame(source[0], 101, field0);
    QCOMPARE(result.status, VideoSubmitStatus::Sent);
    QVERIFY(receiver.field(source[0], 101, field0, true,
        result.timings.transmittedBytes, previous[0]));

    // Reconnect and a mode switch with identical byte size both force full.
    transport.close();
    mode.hActive *= 2;
    mode.vActive /= 2;
    QVERIFY2(receiver.connectTransport(transport, mode, error), error.c_str());
    for (unsigned slot = 0; slot < parities; ++slot) {
        const auto parity = static_cast<FieldParity>(slot);
        result = transport.sendFrame(source[slot], slot + 1, parity);
        QCOMPARE(result.status, VideoSubmitStatus::Sent);
        QVERIFY(receiver.field(source[slot], slot + 1, parity, false,
            result.timings.transmittedBytes, previous[slot]));
    }
}

void GroovyMisterTest::failedSubmissionInvalidatesHistory()
{
    auto mode = testMode;
    mode.interlaced = true;
    mode.vActive *= 2;
    LocalReceiver receiver;
    GroovyMister transport;
    std::string error;
    std::array<std::vector<std::uint8_t>, 2> previous;
    const auto base = randomField(mode.payloadBytes(), 0x12345678);
    const auto candidate = randomField(mode.payloadBytes(), 0x87654321);

    // Failure before header, after header, and after the first payload chunk.
    for (int scenario : {-1, 0, 1}) {
        QVERIFY2(receiver.connectTransport(transport, mode, error), error.c_str());
        for (auto field : {field0, field1}) {
            const auto result = transport.sendFrame(base, 1, field);
            QCOMPARE(result.status, VideoSubmitStatus::Sent);
            QVERIFY(receiver.field(base, 1, field, false, result.timings.transmittedBytes,
                previous[static_cast<unsigned>(field)]));
        }
        const auto invalidations = transport.diagnostics().historyInvalidations;
        payloadBudget = scenario;
        failCommand = scenario == -1;
        const auto result = transport.sendFrame(candidate, 2, field0);
        payloadBudget = -1;
        failCommand = false;
        QCOMPARE(result.status, VideoSubmitStatus::Fatal);
        QCOMPARE(transport.diagnostics().historyInvalidations, invalidations + 1);
        QCOMPARE(transport.diagnostics().fullLz4Sends, std::uint64_t{2});
        QCOMPARE(transport.diagnostics().deltaLz4Sends, std::uint64_t{0});
        QCOMPARE(transport.sendFrame(base, 3, field1).status, VideoSubmitStatus::Fatal);
        if (scenario >= 0) {
            std::array<std::uint8_t, 2048> bytes{};
            QCOMPARE(receiver.receive(bytes), ssize_t{12});
            if (scenario == 1) QCOMPARE(receiver.receive(bytes), ssize_t{1472});
        }
        // Broken streams cannot resume until reconnect; neither parity can
        // retain a candidate or use the previous connection's delta base.
        QVERIFY2(receiver.connectTransport(transport, mode, error), error.c_str());
        for (auto field : {field0, field1}) {
            const auto retry = transport.sendFrame(base, 1, field);
            QCOMPARE(retry.status, VideoSubmitStatus::Sent);
            QVERIFY(receiver.field(base, 1, field, false, retry.timings.transmittedBytes,
                previous[static_cast<unsigned>(field)]));
        }
        transport.close();
    }
}


QTEST_APPLESS_MAIN(GroovyMisterTest)

#include "GroovyMisterTest.moc"
