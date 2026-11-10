#include "core/CrtModeline.h"
#include "core/ModelineCatalog.h"
#include "stream/GroovyProtocolCodec.h"
#include "ui/CrtModeControls.h"

#include <QComboBox>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>

using namespace mistercast;

namespace {

CrtRequest requestFor(const char* preset, int width, int height, double rate, CrtScan scan)
{
    CrtRequest request;
    request.profile = *crtProfile(preset);
    request.width = width;
    request.height = height;
    request.cadenceHz = rate;
    request.scan = scan;
    return request;
}

bool hasText(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

ModelineLoadResult bundledModes()
{
    return ModelineCatalog::load(std::filesystem::path(MISTERCAST_SOURCE_DIR) / "resources/modelines.dat");
}

} // namespace

class CrtModelineTest : public QObject {
    Q_OBJECT

private slots:
    void generatesStandardModes_data();
    void generatesStandardModes();
    void rejectsUnsupportedRequests();
    void respectsCustomRange();
    void rejectsPresetOutsidePllRange();
    void fallsBackToSafePresets();
    void usesManualModesAndOriginalCatalog();
    void usesSourceGeometryWithoutAssumingRefresh();
    void controlsRestoreSettings();
};

void CrtModelineTest::generatesStandardModes_data()
{
    QTest::addColumn<QByteArray>("preset");
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");
    QTest::addColumn<double>("rate");
    QTest::addColumn<bool>("interlaced");
    QTest::addColumn<int>("totalLines");
    QTest::newRow("NTSC 240p60") << QByteArray("ntsc") << 320 << 240 << 60.0 << false << 262;
    QTest::newRow("NTSC 256x240p59.94") << QByteArray("ntsc") << 256 << 240 << 59.94 << false << 262;
    QTest::newRow("NTSC 480i59.94") << QByteArray("ntsc") << 640 << 480 << 59.94 << true << 525;
    QTest::newRow("PAL 576i50") << QByteArray("pal") << 720 << 576 << 50.0 << true << 625;
    QTest::newRow("PAL 288p50") << QByteArray("pal") << 320 << 288 << 50.0 << false << 312;
    QTest::newRow("generic 15 kHz") << QByteArray("generic_15") << 320 << 240 << 60.0 << false << 262;
    QTest::newRow("arcade 15 kHz") << QByteArray("arcade_15") << 320 << 240 << 60.0 << false << 262;
}

void CrtModelineTest::generatesStandardModes()
{
    QFETCH(QByteArray, preset);
    QFETCH(int, width);
    QFETCH(int, height);
    QFETCH(double, rate);
    QFETCH(bool, interlaced);
    QFETCH(int, totalLines);
    const auto request = requestFor(preset.constData(), width, height, rate,
        interlaced ? CrtScan::Interlaced : CrtScan::Progressive);
    const auto result = selectCrtMode(request, kDefaultModeline, {});
    QVERIFY2(result.mode.has_value(), result.error.c_str());
    QCOMPARE(result.selection, CrtSelection::Generated);
    const auto& mode = *result.mode;
    QCOMPARE(mode.hActive, width);
    QCOMPARE(mode.vActive, height);
    QCOMPARE(mode.vTotal, totalLines);
    QCOMPARE(mode.interlaced, interlaced);
    QCOMPARE(mode.payloadHeight(), height / (interlaced ? 2 : 1));
    QVERIFY(validateCrtMode(mode, request).empty());

    const auto expectedHorizontalHz = rate * totalLines / (interlaced ? 2 : 1);
    QVERIFY(std::abs(mode.horizontalHz() - expectedHorizontalHz) < 1e-7);
    QVERIFY(std::abs(mode.cadenceHz() - rate) < 1e-9);

    const auto packet = GroovyProtocolCodec::makeModeSwitchCommand(mode);
    QCOMPARE(packet[0], 3);
    double pixelClock = 0.0;
    std::array<std::uint16_t, 8> wireTiming{};
    std::memcpy(&pixelClock, packet.data() + 1, sizeof(pixelClock));
    std::memcpy(wireTiming.data(), packet.data() + 9, sizeof(wireTiming));
    QCOMPARE(pixelClock, mode.pixelClockMHz);
    const std::array expectedTiming{mode.hActive, mode.hBegin, mode.hEnd, mode.hTotal,
                                    mode.vActive, mode.vBegin, mode.vEnd, mode.vTotal};
    QCOMPARE(wireTiming, expectedTiming);
    QCOMPARE(packet[25], interlaced ? 1 : 0);

    // Independent runs of the pinned Switchres modeline_create with pixel precision.
    if (interlaced) {
        const auto reference = rate == 50.0
            ? std::array<std::uint16_t, 8>{720, 741, 806, 886, 576, 583, 588, 625}
            : std::array<std::uint16_t, 8>{640, 658, 715, 772, 480, 487, 493, 525};
        QCOMPARE(wireTiming, reference);
    }
}

void CrtModelineTest::rejectsUnsupportedRequests()
{
    const auto catalog = bundledModes().modelines;
    auto request = requestFor("ntsc", 1920, 1080, 60.0, CrtScan::Automatic);
    auto result = selectCrtMode(request, kDefaultModeline, catalog, {1920, 1080});
    QVERIFY(!result.mode);
    QVERIFY(hasText(result.error, "720x576"));

    request.width = 720;
    request.height = 576;
    request.scan = CrtScan::Interlaced;
    result = selectCrtMode(request, kDefaultModeline, catalog);
    QVERIFY(!result.mode);
    QVERIFY(hasText(result.error, "No safe CRT modeline"));
    QVERIFY(!result.diagnostics.empty());

    request.height = 480;
    request.cadenceHz = 50.0;
    QVERIFY(!selectCrtMode(request, kDefaultModeline, catalog).mode);
    request.cadenceHz = 59.94;
    request.height = 479;
    QVERIFY(!selectCrtMode(request, kDefaultModeline, catalog).mode);

    // CRU Automatic CRT's DMT entry for 640x480p60 has 800x525 totals.
    // Its 31.5 kHz line rate fails the 15 kHz range even if active lines are allowed.
    const Modeline cruPcTiming{"CRU 640x480p60", 25.2, 640, 656, 752, 800,
                               480, 490, 492, 525, false};
    QCOMPARE(cruPcTiming.horizontalHz(), 31500.0);
    QCOMPARE(cruPcTiming.cadenceHz(), 60.0);
    request = requestFor("ntsc", 640, 480, 60.0, CrtScan::Progressive);
    request.clockMHz.maximum = 25.0;
    request.profile.progressiveLines = {192, 480};
    QVERIFY(hasText(validateCrtMode(cruPcTiming, request), "Horizontal frequency"));
    QVERIFY(!selectCrtMode(request, kDefaultModeline, std::span(&cruPcTiming, 1)).mode);
}

void CrtModelineTest::respectsCustomRange()
{
    auto request = requestFor("ntsc", 640, 480, 59.94, CrtScan::Interlaced);
    request.profile.horizontalHz = {15734.26, 15734.26};
    request.refreshToleranceHz = 0.0;
    QVERIFY(!selectCrtMode(request, kDefaultModeline, {}).mode);
    request.refreshToleranceHz = 0.001;
    auto result = selectCrtMode(request, kDefaultModeline, {});
    QVERIFY2(result.mode.has_value(), result.error.c_str());
    QVERIFY(std::abs(result.mode->horizontalHz() - 15734.26) < 1e-7);
    QVERIFY(std::abs(result.mode->cadenceHz() - 59.94) <= 0.001);

    auto malformed = *result.mode;
    --malformed.vTotal;
    QVERIFY(hasText(validateCrtMode(malformed, request), "odd full-frame"));
    malformed = *result.mode;
    malformed.hBegin = malformed.hActive + 1;
    QVERIFY(hasText(validateCrtMode(malformed, request), "porch"));
    malformed = *result.mode;
    malformed.hTotal += 300;
    malformed.pixelClockMHz = malformed.hTotal * result.mode->horizontalHz() / 1e6;
    QVERIFY(hasText(validateCrtMode(malformed, request), "8-bit"));

    request.profile.horizontalUs = {40.0, 40.0, 40.0};
    result = selectCrtMode(request, kDefaultModeline, {});
    QVERIFY(!result.mode);
    QVERIFY(hasText(result.error, "porch specification"));
}

void CrtModelineTest::fallsBackToSafePresets()
{
    const auto catalog = bundledModes().modelines;
    auto request = requestFor("ntsc", 320, 240, 60.0, CrtScan::Progressive);
    request.clockMHz.minimum = 6.5; // Switchres's shorter H blanking produces about 6.05 MHz.
    request.fallbackName = catalog[1].name;
    auto result = selectCrtMode(request, kDefaultModeline, catalog);
    QVERIFY2(result.mode.has_value(), result.error.c_str());
    QCOMPARE(result.selection, CrtSelection::Fallback);
    QCOMPARE(*result.mode, catalog[1]);
    QVERIFY(hasText(result.diagnostics.back(), "selected fallback"));
    QVERIFY(std::ranges::any_of(result.diagnostics, [](const auto& diagnostic) {
        return hasText(diagnostic, "Pixel clock is outside");
    }));

    request.fallbackName = catalog[6].name; // 320x240p50 is the wrong standard.
    result = selectCrtMode(request, kDefaultModeline, catalog);
    QVERIFY(result.mode);
    QCOMPARE(*result.mode, catalog[1]);
    QVERIFY(hasText(result.diagnostics.back(), "catalog fallback"));
    QVERIFY(std::ranges::any_of(result.diagnostics, [](const auto& diagnostic) {
        return hasText(diagnostic, "Rejected fallback 320x240  PAL");
    }));

    request.fallbackName = "deleted entry";
    result = selectCrtMode(request, kDefaultModeline, {});
    QVERIFY(result.mode);
    QCOMPARE(*result.mode, kDefaultModeline);
    QVERIFY(hasText(result.diagnostics.back(), "built-in fallback"));

    request.clockMHz.maximum = 6.6;
    result = selectCrtMode(request, kDefaultModeline, catalog);
    QVERIFY(!result.mode);
    QVERIFY(hasText(result.error, "No safe CRT modeline"));
}

void CrtModelineTest::rejectsPresetOutsidePllRange()
{
    // The 16x scaler clock is 50.001 MHz. Groovy's PLL rounds it to 50 MHz.
    const Modeline mode{"Narrow custom clock", 50.001 / 16.0, 160, 165, 180, 200,
                        240, 270, 273, 312, false};
    auto request = requestFor("pal", 160, 240, mode.cadenceHz(), CrtScan::Progressive);
    request.profile.preferredHorizontalHz = mode.horizontalHz();
    request.profile.horizontalHz = {mode.horizontalHz(), mode.horizontalHz()};
    QVERIFY(hasText(validateCrtMode(mode, request), "Groovy PLL rounds"));
    request.profile.horizontalHz.minimum = 15625.0;
    QVERIFY(validateCrtMode(mode, request).empty());
}

void CrtModelineTest::usesManualModesAndOriginalCatalog()
{
    const auto catalog = bundledModes();
    QVERIFY(catalog.warnings.empty());
    QCOMPARE(catalog.modelines.size(), 10);
    const auto& original = catalog.modelines.back();
    const Modeline expected{"720x576i PAL (50Hz)", 13.875, 720, 741, 806, 888,
                            576, 581, 586, 625, true};
    QCOMPARE(original, expected);
    const auto result = selectCrtMode(std::nullopt, original, {}, {1920, 1080});
    QCOMPARE(result.selection, CrtSelection::Manual);
    QVERIFY(result.mode);
    QCOMPARE(*result.mode, original);
    QVERIFY(result.diagnostics.empty());
}

void CrtModelineTest::usesSourceGeometryWithoutAssumingRefresh()
{
    auto request = requestFor("ntsc", 0, 0, 0.0, CrtScan::Automatic);
    const auto low = selectCrtMode(request, kDefaultModeline, {}, {256, 240});
    QVERIFY(low.mode);
    QCOMPARE(low.mode->hActive, 256);
    QVERIFY(!low.mode->interlaced);
    const auto hd = selectCrtMode(request, kDefaultModeline, {}, {1920, 1080});
    QVERIFY(hd.mode);
    QCOMPARE(hd.mode->hActive, 640);
    QCOMPARE(hd.mode->vActive, 480);
    QVERIFY(hd.mode->interlaced);
    QVERIFY(std::abs(hd.mode->cadenceHz() - 59.94) < 1e-9);

    request.profile = *crtProfile("pal");
    const auto pal = selectCrtMode(request, kDefaultModeline, {}, {1920, 1080});
    QVERIFY(pal.mode);
    QCOMPARE(pal.mode->hActive, 720);
    QCOMPARE(pal.mode->vActive, 576);
    QVERIFY(std::abs(pal.mode->cadenceHz() - 50.0) < 1e-9);
}

void CrtModelineTest::controlsRestoreSettings()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("MiSTerCast CRT tests"));
    QCoreApplication::setApplicationName(QStringLiteral("Settings"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());

    QSettings settings;
    settings.setValue(QStringLiteral("modeline"), QStringLiteral("existing manual choice"));
    settings.setValue(QStringLiteral("crt/profile"), QStringLiteral("pal"));
    settings.setValue(QStringLiteral("crt/height"), 576);
    settings.setValue(QStringLiteral("crt/refresh"), 50.0);
    settings.setValue(QStringLiteral("crt/fallback"), QStringLiteral("missing preset"));
    {
        CrtModeControls controls;
        controls.setCatalog(bundledModes().modelines);
        QVERIFY(!controls.automatic());
        QVERIFY(!controls.request());
        auto* mode = controls.findChild<QComboBox*>(QStringLiteral("crtResolutionMode"));
        QVERIFY(mode);
        QSignalSpy changed(&controls, &CrtModeControls::settingsChanged);
        mode->setCurrentIndex(1);
        QCOMPARE(changed.count(), 1);
    }

    CrtModeControls reloaded;
    reloaded.setCatalog(bundledModes().modelines);
    const auto request = reloaded.request();
    QVERIFY(request);
    QCOMPARE(request->height, 576);
    QCOMPARE(request->cadenceHz, 50.0);
    QCOMPARE(request->profile.preferredCadenceHz, 50.0);
    QCOMPARE(request->fallbackName, "missing preset");
    QCOMPARE(settings.value(QStringLiteral("modeline")).toString(), QStringLiteral("existing manual choice"));
}

QTEST_MAIN(CrtModelineTest)
#include "CrtModelineTest.moc"
