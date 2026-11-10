#include "core/AudioFrameCadence.h"
#include "core/CrtModeline.h"
#include "stream/GroovyMister.h"
#include "stream/StreamController.h"

#include <QScopeGuard>
#include <QtTest>

#include <arpa/inet.h>
#include <cstring>
#include <lz4.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace mistercast;

namespace {

Modeline generate(const char* profile, int width, int height, double cadence, CrtScan scan)
{
    CrtRequest request;
    request.profile = *crtProfile(profile);
    request.width = width;
    request.height = height;
    request.cadenceHz = cadence;
    request.scan = scan;
    return selectCrtMode(request, kDefaultModeline, {}).mode.value();
}

class LoopbackReceiver {
public:
    explicit LoopbackReceiver(int socket) : socket_(socket) {}

    bool readable(int timeout) const
    {
        pollfd descriptor{socket_, POLLIN, 0};
        return poll(&descriptor, 1, timeout) > 0;
    }

    std::span<const std::uint8_t> receive()
    {
        if (!readable(1000)) {
            return {};
        }
        socklen_t length = sizeof(peer_);
        const auto bytes = recvfrom(socket_, packet_.data(), packet_.size(), 0,
            reinterpret_cast<sockaddr*>(&peer_), &length);
        return bytes > 0 ? std::span(packet_).first(bytes) : std::span<const std::uint8_t>{};
    }

    void receiveMode(const Modeline& mode)
    {
        const auto init = receive();
        QVERIFY(std::ranges::equal(init, GroovyProtocolCodec::makeInitCommand(false)));
        GroovyProtocolCodec::StatusPacket ack{};
        ack.back() = 0x47;
        QCOMPARE(sendto(socket_, ack.data(), ack.size(), 0,
            reinterpret_cast<const sockaddr*>(&peer_), sizeof(peer_)), ack.size());
        QVERIFY(std::ranges::equal(receive(), GroovyProtocolCodec::makeModeSwitchCommand(mode)));
    }

    int receiveField(const Modeline& mode, bool first)
    {
        const auto blit = receive();
        if ((blit.size() != 12 && blit.size() != 13) || blit[0] != 7 ||
            (first && blit.size() != 12) || blit[5] > 1) {
            return -1;
        }
        const auto field = blit[5];
        std::uint16_t syncLine = 0;
        std::memcpy(&syncLine, blit.data() + 6, sizeof(syncLine));
        if (first && syncLine != mode.vTotal / 2) {
            return -1;
        }
        std::uint32_t size = 0;
        std::memcpy(&size, blit.data() + 8, sizeof(size));
        std::vector<char> compressed;
        while (compressed.size() < size) {
            const auto packet = receive();
            if (packet.empty()) {
                return -1;
            }
            compressed.insert(compressed.end(), packet.begin(), packet.end());
        }
        std::vector<char> unpacked(mode.payloadBytes());
        if (compressed.size() != size ||
            LZ4_decompress_safe(compressed.data(), unpacked.data(), size, unpacked.size()) !=
                static_cast<int>(unpacked.size()) ||
            unpacked.front() != (mode.interlaced ? 2 - field : 1)) {
            return -1;
        }
        return field;
    }

    bool receiveClose()
    {
        for (int packets = 0; packets < 32; ++packets) {
            const auto packet = receive();
            if (packet.empty()) {
                return false;
            }
            if (packet.size() == 1 && packet[0] == 1) {
                return true;
            }
        }
        return false;
    }

private:
    int socket_;
    sockaddr_in peer_{};
    std::array<std::uint8_t, 2048> packet_{};
};

void publish(FrameMailbox& mailbox, const Modeline& mode, ModeEpoch epoch)
{
    std::vector<std::uint8_t> frame(mode.fullFrameBytes());
    const std::size_t rowBytes = mode.hActive * 3;
    for (std::size_t row = 0; row < mode.vActive; ++row) {
        std::fill_n(frame.begin() + row * rowBytes, rowBytes, row % 2 + 1);
    }
    mailbox.publish(frame, mode.hActive, mode.vActive, epoch);
}

} // namespace

class CrtStreamTest : public QObject {
    Q_OBJECT

private slots:
    void restartWithGeneratedGeometry();
};

void CrtStreamTest::restartWithGeneratedGeometry()
{
    const int server = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    QVERIFY(server >= 0);
    const auto closeSocket = qScopeGuard([server] { close(server); });
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(GroovyMister::kDefaultPort);
    QVERIFY(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    const auto ntsc = generate("ntsc", 320, 240, 60.0, CrtScan::Progressive);
    const auto pal = generate("pal", 720, 576, 50.0, CrtScan::Interlaced);
    const ModeEpoch oldEpoch{10};
    const ModeEpoch newEpoch{11};
    auto mailbox = std::make_unique<FrameMailbox>();
    PipeWireAudioCapture audioCapture;
    StreamController controller(*mailbox, audioCapture);
    LoopbackReceiver receiver(server);

    publish(*mailbox, ntsc, oldEpoch);
    QVERIFY(controller.start({"127.0.0.1", false, ntsc, oldEpoch, false, {}, false}));
    receiver.receiveMode(ntsc);
    QCOMPARE(receiver.receiveField(ntsc, true), 0);
    controller.stop();
    QVERIFY(receiver.receiveClose());

    // The previous session is joined. Matching dimensions with a stale epoch
    // still cannot trigger CMD_INIT or transmit any part of the new mode.
    publish(*mailbox, pal, oldEpoch);
    QVERIFY(controller.start({"127.0.0.1", false, pal, newEpoch, false, {}, false}));
    QVERIFY(!receiver.readable(120));
    publish(*mailbox, pal, newEpoch);
    receiver.receiveMode(pal);
    const auto firstField = receiver.receiveField(pal, true);
    QVERIFY(firstField >= 0);
    QCOMPARE(receiver.receiveField(pal, false), 1 - firstField);
    controller.stop();
    QVERIFY(receiver.receiveClose());

    AudioFrameCadence ntscPacing(ntsc);
    AudioFrameCadence palPacing(pal);
    QCOMPARE(ntscPacing.next(), 800);
    QCOMPARE(palPacing.next(), 960);
}

QTEST_GUILESS_MAIN(CrtStreamTest)
#include "CrtStreamTest.moc"
