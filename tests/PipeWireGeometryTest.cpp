#include "capture/PipeWireVideoCapture.h"
#include "core/FrameFields.h"
#include "stream/GroovyVideoEncoder.h"
#include "IccFixtures.h"

#include <lz4.h>

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
    void staticSourceViewport();
    void iccStaticInterlaced();
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
    const CrtGeometrySettings geometry{true, 1, 2, 1, -1, true};
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

    auto cabinetOff = geometry;
    cabinetOff.cabinetEnabled = false;
    QCOMPARE(rerendered(cabinetOff), FrameReadResult::Copied);
    const std::vector<std::uint8_t> ordinary(frame->begin(), frame->begin() + spec.bytes);
    QVERIFY(ordinary != edited && ordinary != original);
    const auto beforeDormantEdit = meta.processing.settingsRevision;
    auto dormantEdit = cabinetOff;
    dormantEdit.cabinetVSize = 3;
    capture->setGeometrySettings(dormantEdit);
    QCOMPARE(capture->settingsSnapshot().revision, beforeDormantEdit);
    for (int i = 0; i < 12; ++i) {
        QCOMPARE(rerendered(geometry), FrameReadResult::Copied);
        QVERIFY(std::equal(edited.begin(), edited.end(), frame->begin()));
        QCOMPARE(rerendered(cabinetOff), FrameReadResult::Copied);
        QVERIFY(std::equal(ordinary.begin(), ordinary.end(), frame->begin()));
        QCOMPARE(meta.modeEpoch, epoch);
        QCOMPARE(meta.capturedAt, at);
        QCOMPARE(meta.processing.conversion.count(), 0);
        QCOMPARE(meta.processing.failures, 0u);
    }

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

    // The CRT offset applies after
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

void PipeWireGeometryTest::staticSourceViewport()
{
    auto mailbox = std::make_unique<FrameMailbox>();
    auto capture = std::make_unique<PipeWireVideoCapture>(*mailbox);
    auto frame = std::make_unique<std::array<std::uint8_t, kMaximumFrameBytes>>();
    auto mode = kDefaultModeline;
    mode.hActive = 20;
    mode.vActive = 16;
    mode.interlaced = true;
    const auto epoch = capture->setOutputMode(mode, false);
    QVERIFY(startLoop(*capture));
    const FrameSpec spec{mode.fullFrameBytes(), 20, 16, epoch};
    std::vector<std::uint8_t> original(16 * 12 * 3, 211);
    const SourceFrame source{original.data() + 16 * 11 * 3, 16, 12, -16 * 3, PixelFormat::Bgr};
    capture->convertSource(&source, FrameMailbox::Clock::now());
    const auto* cache = capture->sourceStorage_.get();
    QVERIFY(cache);
    FrameCursor cursor;
    FrameMetadata meta;
    QCOMPARE(mailbox->tryReadLatest(*frame, cursor, meta, spec), FrameReadResult::Copied);
    const auto revision = meta.processing.settingsRevision;

    std::ranges::fill(original, 13); // The compositor can overwrite a returned buffer.
    capture->setContentViewport({true, false, 16, 12, 3, 1});
    QCOMPARE(mailbox->waitForLatest(*frame, cursor, meta, spec, std::chrono::seconds(1)), FrameReadResult::Copied);
    QCOMPARE(meta.modeEpoch, epoch);
    QVERIFY(meta.processing.settingsRevision > revision);
    QCOMPARE((*frame)[(20 + 3) * 3], 211);
    QCOMPARE((*frame)[(20 + 2) * 3], 0);
    for (int i = 0; i < 64; ++i) capture->setContentViewport({true, false, 16, 12, -i, -1});
    capture->setContentViewport({true, true, 16, 12});
    const auto finalRevision = capture->settingsSnapshot().revision;
    while (meta.processing.settingsRevision < finalRevision) {
        const auto before = meta.processing.settingsRevision;
        QCOMPARE(mailbox->waitForLatest(*frame, cursor, meta, spec, std::chrono::seconds(1)), FrameReadResult::Copied);
        QVERIFY(meta.processing.settingsRevision > before);
    }
    QCOMPARE((*frame)[(2 * 20 + 2) * 3], 211);
    QCOMPARE(capture->sourceStorage_.get(), cache);
    capture->convertSource(&source, FrameMailbox::Clock::now());
    QCOMPARE(capture->sourceStorage_.get(), cache);
    capture->stop();
    QVERIFY(!capture->cachedSource_);
    QVERIFY(startLoop(*capture));
    capture->setContentViewport({});
    QCOMPARE(mailbox->tryReadLatest(*frame, cursor, meta, spec), FrameReadResult::Incompatible);
    QCOMPARE(mailbox->waitForLatest(*frame, cursor, meta, spec, std::chrono::milliseconds(50)), FrameReadResult::NoChange);
    capture->stop();
}

void PipeWireGeometryTest::iccStaticInterlaced()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path base(dir.path().toStdString());
    const auto matrix = base / "matrix.icc";
    const auto lut = base / "lut.icm";
    QVERIFY(icc_test::writeDisplay(matrix, false, false));
    QVERIFY(icc_test::writeDisplay(lut, true, true));
    const auto first = IccTransform::load(matrix);
    auto second = IccTransform::load(lut);
    QVERIFY(first.transform && second.transform);

    auto mailbox = std::make_unique<FrameMailbox>();
    auto capture = std::make_unique<PipeWireVideoCapture>(*mailbox);
    auto frame = std::make_unique<std::array<std::uint8_t, kMaximumFrameBytes>>();
    const Modeline mode{.name = {}, .hActive = 64, .vActive = 48, .interlaced = true};
    const CrtGeometrySettings geometry{true, 5, 3, 1, -2, true};
    const auto epoch = capture->setOutputMode(mode, false, geometry);
    const FrameSpec spec{mode.fullFrameBytes(), mode.hActive, mode.vActive, epoch};
    QVERIFY(startLoop(*capture));

    std::vector<std::uint8_t> image(spec.bytes), geometryOnly(spec.bytes), expected(spec.bytes);
    const auto stride = mode.hActive * 3;
    for (std::size_t i = 0; i < image.size(); ++i) {
        image[i] = (i / stride) % 2 ? (i * 31) % 256 : (i * 47 + 5) % 256;
    }
    const SourceFrame source{image.data(), mode.hActive, mode.vActive, stride, PixelFormat::Bgr};
    const auto at = FrameMailbox::Clock::now();
    capture->convertSource(&source, at);
    FrameCursor cursor;
    FrameMetadata meta;
    QCOMPARE(mailbox->tryReadLatest(*frame, cursor, meta, spec), FrameReadResult::Copied);
    QVERIFY(!capture->iccOutput_);
    const auto plan = CrtGeometryPlan::make(mode, geometry);
    QVERIFY(plan);
    plan->render(image, geometryOnly, 0, mode.vActive);
    QVERIFY(std::equal(geometryOnly.begin(), geometryOnly.end(), frame->begin()));

    const auto rerender = [&](IccProcessingSettings settings) {
        capture->setIccSettings(std::move(settings));
        return mailbox->waitForLatest(*frame, cursor, meta, spec, std::chrono::seconds(1));
    };
    QCOMPARE(rerender({first.transform, false}), FrameReadResult::Copied);
    QVERIFY(meta.processing.icc.count() > 0);
    QCOMPARE(meta.processing.conversion.count(), 0);
    QCOMPARE(meta.capturedAt, at);
    QVERIFY(first.transform->apply(geometryOnly, expected, mode.hActive, mode.vActive, false));
    QVERIFY(std::equal(expected.begin(), expected.end(), frame->begin()));
    QVERIFY(expected != geometryOnly);
    const auto* iccBuffer = capture->iccOutput_.get();
    const auto beforeReplacement = meta.processing.settingsRevision;
    QCOMPARE(rerender({second.transform, true}), FrameReadResult::Copied);
    QVERIFY(meta.processing.settingsRevision > beforeReplacement);
    QCOMPARE(meta.modeEpoch, epoch);
    QVERIFY(second.transform->apply(geometryOnly, expected, mode.hActive, mode.vActive, true));
    QVERIFY(std::equal(expected.begin(), expected.end(), frame->begin()));

    // Exercise the sender's unchanged field extractor and encoder for both
    // parities across a correction update. Compression is allowed to choose
    // either a full payload or a delta based on content.
    GroovyVideoEncoder encoder;
    std::vector<std::uint8_t> raw(mode.payloadBytes()), previous(mode.payloadBytes()), decoded(mode.payloadBytes());
    for (auto parity : {FieldParity::Field0OddSourceLines, FieldParity::Field1EvenSourceLines}) {
        QVERIFY(extractInterlacedField(geometryOnly, mode, parity, previous));
        const auto before = encoder.prepare(previous, mode, parity);
        encoder.commit(previous, parity, before);
        QVERIFY(extractInterlacedField(std::span(*frame).first(spec.bytes), mode, parity, raw));
        const std::size_t firstRow = parity == FieldParity::Field0OddSourceLines ? 1 : 0;
        for (std::size_t y = 0; y < mode.payloadHeight(); ++y) {
            QVERIFY(std::equal(raw.begin() + y * stride, raw.begin() + (y + 1) * stride,
                               expected.begin() + (2 * y + firstRow) * stride));
        }
        const auto after = encoder.prepare(raw, mode, parity);
        QCOMPARE(LZ4_decompress_safe(reinterpret_cast<const char*>(after.payload.data()),
                                     reinterpret_cast<char*>(decoded.data()),
                                     after.payload.size(), decoded.size()), int(decoded.size()));
        if (after.delta) {
            for (std::size_t i = 0; i < decoded.size(); ++i) decoded[i] += previous[i];
        }
        QCOMPARE(decoded, raw);
        encoder.commit(raw, parity, after);
    }

    const ContentViewport shifted{true, false, 64, 48, -3, 1};
    capture->setContentViewport(shifted);
    QCOMPARE(mailbox->waitForLatest(*frame, cursor, meta, spec, std::chrono::seconds(1)), FrameReadResult::Copied);
    std::vector<std::uint8_t> padded(spec.bytes), adjusted(spec.bytes);
    for (int row = 0; row < mode.vActive - 1; ++row) {
        std::copy_n(image.begin() + row * stride + 9, stride - 9, padded.begin() + (row + 1) * stride);
    }
    plan->render(padded, adjusted, 0, mode.vActive);
    QVERIFY(second.transform->apply(adjusted, expected, mode.hActive, mode.vActive, true));
    QCOMPARE(meta.modeEpoch, epoch);
    QVERIFY(std::equal(expected.begin(), expected.end(), frame->begin()));
    capture->setContentViewport({});
    QCOMPARE(mailbox->waitForLatest(*frame, cursor, meta, spec, std::chrono::seconds(1)), FrameReadResult::Copied);

    for (int i = 0; i < 100; ++i) {
        capture->setIccSettings({first.transform, false});
        capture->setIccSettings({second.transform, true});
        capture->setIccSettings({});
    }
    const auto latest = capture->settingsSnapshot().revision;
    QElapsedTimer timer;
    timer.start();
    while (meta.processing.settingsRevision != latest && timer.elapsed() < 1000) {
        mailbox->waitForLatest(*frame, cursor, meta, spec, std::chrono::milliseconds(50));
    }
    QCOMPARE(meta.processing.settingsRevision, latest);
    QCOMPARE(meta.processing.icc.count(), 0);
    QVERIFY(std::equal(geometryOnly.begin(), geometryOnly.end(), frame->begin()));
    QCOMPARE(capture->iccOutput_.get(), iccBuffer);
    capture->setIccSettings({std::move(second.transform), true});
    capture->stop();
    QVERIFY(startLoop(*capture));
    QCOMPARE(mailbox->tryReadLatest(*frame, cursor, meta, spec), FrameReadResult::Incompatible);
    capture->setIccSettings({});
    QCOMPARE(mailbox->waitForLatest(*frame, cursor, meta, spec, std::chrono::milliseconds(40)),
             FrameReadResult::NoChange);
    capture->stop();
}

} // namespace mistercast

QTEST_GUILESS_MAIN(mistercast::PipeWireGeometryTest)
#include "PipeWireGeometryTest.moc"
