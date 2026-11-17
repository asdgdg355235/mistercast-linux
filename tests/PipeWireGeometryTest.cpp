#include "capture/PipeWireVideoCapture.h"

#include <QtTest>

#include <memory>
#include <vector>

namespace mistercast {

class PipeWireGeometryTest : public QObject {
    Q_OBJECT

    static bool startLoop(PipeWireVideoCapture& capture)
    {
        capture.loop_ = pw_thread_loop_new("geometry-test", nullptr);
        if (!capture.loop_ || pw_thread_loop_start(capture.loop_) < 0) return false;
        capture.loopStarted_ = true;
        capture.running_ = true;
        return true;
    }

private slots:
    void staticSourceAndModeEpoch();
};

void PipeWireGeometryTest::staticSourceAndModeEpoch()
{
    auto mailbox = std::make_unique<FrameMailbox>();
    auto capture = std::make_unique<PipeWireVideoCapture>(*mailbox);
    auto frame = std::make_unique<std::array<std::uint8_t, kMaximumFrameBytes>>();
    auto mode = kDefaultModeline;
    mode.hActive = 16;
    mode.vActive = 12;
    const auto epoch = capture->setOutputMode(mode, false);
    QVERIFY(startLoop(*capture));
    const FrameSpec spec{mode.fullFrameBytes(), 16, 12, epoch};
    std::vector<std::uint8_t> image(spec.bytes), original(spec.bytes);
    for (std::size_t i = 0; i < image.size(); ++i) image[i] = (i * 53 + 7) % 255 + 1;
    const SourceFrame source{image.data(), 16, 12, 16*3, PixelFormat::Rgb};
    const auto at = FrameMailbox::Clock::now();
    capture->convertSource(&source, at);
    FrameCursor cursor;
    FrameMetadata meta;
    QCOMPARE(mailbox->tryReadLatest(*frame, cursor, meta, spec), FrameReadResult::Copied);
    std::copy_n(frame->begin(), spec.bytes, original.begin());
    QCOMPARE(original.front(), image[2]);

    const auto rerendered = [&](CrtGeometrySettings settings) {
        capture->setGeometrySettings(settings);
        return mailbox->waitForLatest(*frame, cursor, meta, spec, std::chrono::seconds(1));
    };
    const CrtGeometrySettings geometry{true, 1, 2, 1, -1};
    QCOMPARE(rerendered(geometry), FrameReadResult::Copied);
    QCOMPARE(meta.processing.origin, FrameOrigin::GeometryRerender);
    QVERIFY(meta.capturedAt == at);
    QVERIFY(meta.processing.conversion.count() == 0);
    QVERIFY(meta.processing.geometry.count() > 0);
    QVERIFY(!std::equal(original.begin(), original.end(), frame->begin()));
    const std::vector<std::uint8_t> edited(frame->begin(), frame->begin() + spec.bytes);
    const auto revision = meta.processing.settingsRevision;
    QCOMPARE(rerendered({}), FrameReadResult::Copied);
    QVERIFY(std::equal(original.begin(), original.end(), frame->begin()));
    QVERIFY(meta.processing.settingsRevision > revision);
    QCOMPARE(meta.processing.geometry.count(), 0);
    QCOMPARE(rerendered(geometry), FrameReadResult::Copied);
    QVERIFY(std::equal(edited.begin(), edited.end(), frame->begin()));

    for (int i = 0; i < 400; ++i) {
        capture->setGeometrySettings({true, i % 15, i % 48, 0, 0});
    }
    capture->setGeometrySettings({});
    const auto latest = capture->settingsSnapshot().revision;
    auto lastRevision = meta.processing.settingsRevision;
    QElapsedTimer timer;
    timer.start();
    while (lastRevision != latest && timer.elapsed() < 1000) {
        if (mailbox->waitForLatest(*frame, cursor, meta, spec, std::chrono::milliseconds(50)) ==
            FrameReadResult::Copied) {
            QVERIFY(meta.processing.settingsRevision >= lastRevision);
            lastRevision = meta.processing.settingsRevision;
        }
    }
    QCOMPARE(lastRevision, latest);
    QVERIFY(std::equal(original.begin(), original.end(), frame->begin()));

    mode.vActive = 10;
    const auto nextEpoch = capture->setOutputMode(mode, true, {true, 0, 2});
    FrameSpec nextSpec{mode.fullFrameBytes(), 16, 10, nextEpoch};
    QVERIFY(nextEpoch != epoch);
    QCOMPARE(mailbox->tryReadLatest(*frame, cursor, meta, nextSpec), FrameReadResult::Copied);
    QCOMPARE(meta.processing.origin, FrameOrigin::ModePreview);
    QVERIFY(meta.capturedAt == at);
    QCOMPARE(frame->front(), 0);
    QCOMPARE((*frame)[6], original.front());

    // A failed conversion cannot turn the previous raster into a new revision.
    auto invalid = source;
    invalid.stride = 1;
    capture->convertSource(&invalid, at);
    QVERIFY(!capture->baseline_);
    capture->setGeometrySettings({true, 0, -3});
    QCOMPARE(mailbox->waitForLatest(*frame, cursor, meta, nextSpec, std::chrono::milliseconds(40)),
             FrameReadResult::NoChange);

    // A format reset (including same output-mode renegotiation) retires the source cache.
    capture->convertSource(&source, at);
    QCOMPARE(mailbox->tryReadLatest(*frame, cursor, meta, nextSpec), FrameReadResult::Copied);
    capture->handleParamChanged(SPA_PARAM_Format, nullptr);
    capture->setGeometrySettings({});
    QCOMPARE(mailbox->waitForLatest(*frame, cursor, meta, nextSpec, std::chrono::milliseconds(40)),
             FrameReadResult::NoChange);

    // Framing changes require new source pixels. The CRT offset applies after
    // rotation, so shifting right still puts black on the left.
    FramingSettings framing;
    framing.rotation = Rotation::Flip180;
    capture->setFramingSettings(framing);
    capture->convertSource(&source, at);
    QCOMPARE(mailbox->tryReadLatest(*frame, cursor, meta, nextSpec), FrameReadResult::Copied);
    const auto rotatedFirst = frame->front();
    capture->setGeometrySettings({true, 0, 1});
    QCOMPARE(mailbox->waitForLatest(*frame, cursor, meta, nextSpec, std::chrono::seconds(1)),
             FrameReadResult::Copied);
    QCOMPARE(frame->front(), 0);
    QCOMPARE((*frame)[3], rotatedFirst);

    // Stop with queued work and verify a restart cannot render the old cache.
    for (int i = 0; i < 10; ++i) {
        capture->setGeometrySettings({true, i, 3});
        capture->stop();
        QVERIFY(startLoop(*capture));
        QVERIFY(!capture->baseline_);
    }
    capture->stop();
}

} // namespace mistercast

QTEST_GUILESS_MAIN(mistercast::PipeWireGeometryTest)
#include "PipeWireGeometryTest.moc"
