#include "core/FrameProcessor.h"
#include "ui/ScalingSelector.h"

#include <QComboBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <array>
#include <set>
#include <vector>

using namespace mistercast;

namespace {
std::vector<std::uint8_t> gray(std::initializer_list<int> values)
{
    std::vector<std::uint8_t> result;
    for (const auto v : values) for (int c = 0; c < 3; ++c) result.push_back(v);
    return result;
}
}

class ScalerTest final : public QObject {
    Q_OBJECT
private slots:
    void registryAndSettings();
    void resizeGoldens();
    void cropRotationFormatsAndStride();
    void nearestFractionalRotation();
    void partitions();
    void desktopFractionalReduction();
    void invalidInputs();
};

void ScalerTest::registryAndSettings()
{
    const auto registry = availableScalers();
    QCOMPARE(registry.size(), std::size_t{3});
    const std::array algorithms{ScalingAlgorithm::Nearest, ScalingAlgorithm::Bilinear, ScalingAlgorithm::Area};
    const std::array<std::string_view, 3> keys{"nearest", "bilinear", "area"};
    std::set<std::string_view> unique;
    for (std::size_t i = 0; i < registry.size(); ++i) {
        QCOMPARE(registry[i].algorithm, algorithms[i]);
        QVERIFY(registry[i].key == keys[i]);
        QVERIFY(scalingAlgorithmFromKey(keys[i]) == algorithms[i]);
        QVERIFY(unique.insert(registry[i].key).second);
        QVERIFY(scalerDescriptor(algorithms[i]).key == keys[i]);
    }
    QVERIFY(!scalingAlgorithmFromKey("unknown"));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath("settings.ini");
    QSettings settings(path, QSettings::IniFormat);
    QComboBox combo;
    populateScalingSelector(combo, settings);
    QCOMPARE(selectedScalingAlgorithm(combo), ScalingAlgorithm::Nearest);
    settings.setValue("scalingAlgorithm", "unknown");
    populateScalingSelector(combo, settings);
    QCOMPARE(selectedScalingAlgorithm(combo), ScalingAlgorithm::Nearest);
    QCOMPARE(combo.count(), static_cast<int>(registry.size()));
    for (int i = 0; i < combo.count(); ++i) {
        QCOMPARE(combo.itemText(i).toStdString(), std::string(registry[i].displayName));
        QCOMPARE(combo.itemData(i).toString().toStdString(), std::string(registry[i].key));
    }
    // Reorder the actual widget: persistence and typed selection use keys.
    combo.clear();
    for (auto it = registry.rbegin(); it != registry.rend(); ++it) {
        combo.addItem(QString::fromUtf8(it->displayName.data(), it->displayName.size()),
                      QString::fromUtf8(it->key.data(), it->key.size()));
    }
    for (const auto& descriptor : registry) {
        const auto key = QString::fromUtf8(descriptor.key.data(), descriptor.key.size());
        combo.setCurrentIndex(combo.findData(key));
        QCOMPARE(selectedScalingAlgorithm(combo), descriptor.algorithm);
        saveScalingSelection(combo, settings);
        settings.sync();
        QSettings reopened(path, QSettings::IniFormat);
        QCOMPARE(reopened.value("scalingAlgorithm").toString(), key);
        QComboBox restored;
        populateScalingSelector(restored, reopened);
        QCOMPARE(selectedScalingAlgorithm(restored), descriptor.algorithm);
    }
    combo.setCurrentIndex(-1);
    QCOMPARE(selectedScalingAlgorithm(combo), ScalingAlgorithm::Nearest);
}

void ScalerTest::resizeGoldens()
{
    const auto check = [](const std::vector<std::uint8_t>& source, int sw, int sh,
                          int ow, int oh, ScalingAlgorithm algorithm,
                          const std::vector<std::uint8_t>& expected) {
        std::vector<std::uint8_t> output(ow * oh * 3);
        QVERIFY(FrameProcessor::resizeBgr(source, sw, sh, output, ow, oh, algorithm));
        QCOMPARE(output, expected);
    };
    for (const auto& descriptor : availableScalers()) {
        check({1,2,3, 4,5,6, 7,8,9, 250,251,252}, 2,2,2,2, descriptor.algorithm,
              {1,2,3, 4,5,6, 7,8,9, 250,251,252});
    }
    check(gray({0,100}), 2,1,4,1, ScalingAlgorithm::Nearest, gray({0,0,100,100}));
    for (const auto algorithm : {ScalingAlgorithm::Bilinear, ScalingAlgorithm::Area}) {
        check(gray({0,100}), 2,1,4,1, algorithm, gray({0,25,75,100}));
        check(gray({0,100}), 1,2,1,4, algorithm, gray({0,25,75,100}));
        check(gray({0,60,120,180}), 2,2,3,3, algorithm,
              gray({0,30,60,60,90,120,120,150,180}));
        check(gray({17}), 1,1,3,2, algorithm, gray({17,17,17,17,17,17}));
    }
    check(gray({0,4,8,12, 16,20,24,28, 32,36,40,44, 48,52,56,60}),
          4,4,2,2, ScalingAlgorithm::Area, gray({10,18,42,50}));
    // 5/3-wide intervals: weights (3,2)/5, (1,3,1)/5, (2,3)/5.
    check({0,100,200, 10,90,180, 30,70,140, 60,40,80, 100,0,0},
          5,1,3,1, ScalingAlgorithm::Area, {4,96,192, 32,68,136, 84,16,32});
    // Fractional coverage in both axes, plus half-up rounding.
    check(gray({0,0,90, 0,90,0, 90,0,0}), 3,3,2,2, ScalingAlgorithm::Area,
          gray({10,50,50,10}));
    check(gray({0,1}), 2,1,1,1, ScalingAlgorithm::Area, gray({1}));
    // Horizontal area reduction with vertical bilinear enlargement.
    check(gray({0,30,90, 60,90,150}), 3,2,2,3, ScalingAlgorithm::Area,
          gray({10,70,40,100,70,130}));
}

void ScalerTest::cropRotationFormatsAndStride()
{
    const std::array rotations{Rotation::None, Rotation::Clockwise90,
                              Rotation::CounterClockwise90, Rotation::Flip180};
    const std::array<std::array<int,12>,4> filtered{{
        {16,24,32,40,56,64,72,80,96,104,112,120},
        {136,96,56,16,144,104,64,24,152,112,72,32},
        {32,72,112,152,24,64,104,144,16,56,96,136},
        {120,112,104,96,80,72,64,56,40,32,24,16}}};
    const std::array<std::array<int,12>,4> nearest{{
        {4,12,20,28,44,52,60,68,84,92,100,108},
        {124,84,44,4,132,92,52,12,140,100,60,20},
        {20,60,100,140,12,52,92,132,4,44,84,124},
        {108,100,92,84,68,60,52,44,28,20,12,4}}};
    for (const auto& descriptor : availableScalers()) {
        for (std::size_t rotation = 0; rotation < rotations.size(); ++rotation) {
            const bool quarter = rotation == 1 || rotation == 2;
            const int width = quarter ? 8 : 10, height = quarter ? 8 : 6;
            for (const auto format : {PixelFormat::Bgr, PixelFormat::Bgrx, PixelFormat::Bgra,
                                      PixelFormat::Rgb, PixelFormat::Rgbx, PixelFormat::Rgba}) {
                const bool rgb = format == PixelFormat::Rgb || format == PixelFormat::Rgbx || format == PixelFormat::Rgba;
                const int bpp = format == PixelFormat::Bgr || format == PixelFormat::Rgb ? 3 : 4;
                const int stride = width * bpp + 7;
                for (const bool negative : {false, true}) {
                    std::vector<std::uint8_t> storage(stride * height, 255);
                    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                        auto* pixel = storage.data() + (negative ? height - 1 - y : y) * stride + x * bpp;
                        pixel[rgb ? 2 : 0] = y * 20 + x * 4;
                        pixel[1] = y * 20 + x * 4 + 10;
                        pixel[rgb ? 0 : 2] = y * 20 + x * 4 + 20;
                    }
                    const SourceFrame source{storage.data() + (negative ? (height - 1) * stride : 0),
                        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                        negative ? -stride : stride, format};
                    FramingSettings settings;
                    settings.scaling = descriptor.algorithm;
                    settings.rotation = rotations[rotation];
                    settings.horizontal = HorizontalAlignment::Right;
                    settings.offsetX = -1; // Both geometries have two discarded columns.
                    std::array<std::uint8_t,36> output{};
                    QVERIFY(FrameProcessor::convertToBgr(source, output, 4, 3, settings));
                    const auto& expected = descriptor.algorithm == ScalingAlgorithm::Nearest ? nearest[rotation] : filtered[rotation];
                    for (int p = 0; p < 12; ++p) for (int c = 0; c < 3; ++c) {
                        QCOMPARE(output[p * 3 + c], expected[p] + c * 10);
                    }
                }
            }
        }
    }
    // Vertical alignment and offset: a 4x6 source cropped to rows 2..4.
    std::vector<std::uint8_t> source(4 * 6 * 3);
    for (int y = 0; y < 6; ++y) std::fill_n(source.data() + y * 12, 12, y * 20);
    for (const auto& descriptor : availableScalers()) {
        FramingSettings settings;
        settings.scaling = descriptor.algorithm;
        settings.vertical = VerticalAlignment::Bottom;
        settings.offsetY = -1;
        std::array<std::uint8_t,36> out{};
        QVERIFY(FrameProcessor::convertToBgr({source.data(),4,6,0,PixelFormat::Bgr}, out,4,3,settings));
        QCOMPARE(out[0], 40); QCOMPARE(out[12], 60); QCOMPARE(out[24], 80);
    }
}

void ScalerTest::nearestFractionalRotation()
{
    std::vector<std::uint8_t> source(5 * 4 * 3);
    for (int p = 0; p < 20; ++p) source[p * 3] = p;
    ScalePlan plan;
    QVERIFY(prepareScalePlan({source.data(),5,4,0,PixelFormat::Bgr},
        {0,0,5,4,Rotation::Flip180,3,2}, ScalingAlgorithm::Nearest, plan));
    std::array<std::uint8_t,18> out{};
    QVERIFY(scaleRows(plan,out,0,2));
    const std::array<int,6> expected{13,11,10,3,1,0};
    for (int p = 0; p < 6; ++p) QCOMPARE(out[p*3],expected[p]);
}

void ScalerTest::partitions()
{
    std::vector<std::uint8_t> source(37 * 29 * 3);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = (i * 73 + 19) % 256;
    for (const auto& descriptor : availableScalers()) {
        for (const auto rotation : {Rotation::None,Rotation::Clockwise90,Rotation::CounterClockwise90,Rotation::Flip180}) {
            for (const auto width : {13,45}) {
                FramingSettings settings;
                settings.rotation = rotation;
                settings.scaling = descriptor.algorithm;
                ScalePlan plan;
                const SourceFrame frame{source.data(),37,29,111,PixelFormat::Bgr};
                QVERIFY(FrameProcessor::prepareConversion(frame,width,17,settings,plan));
                std::vector<std::uint8_t> whole(width*17*3), split(whole.size(), 0xcd);
                QVERIFY(FrameProcessor::convertToBgr(frame,whole,width,17,settings));
                QVERIFY(scaleRows(plan,split,9,17));
                QVERIFY(std::all_of(split.begin(),split.begin()+width*9*3,[](auto v){return v==0xcd;}));
                QVERIFY(scaleRows(plan,split,0,3));
                QVERIFY(scaleRows(plan,split,3,9));
                QVERIFY(scaleRows(plan,split,7,7));
                QCOMPARE(split,whole);
            }
        }
    }
}

void ScalerTest::desktopFractionalReduction()
{
    // 1920/720 = 8/3 and 1440/576 = 5/2. Repeated cells make
    // every destination phase independently checkable without a reference scaler.
    std::vector<std::uint8_t> source(1920 * 1440 * 3);
    for (int y = 0; y < 1440; ++y) for (int x = 0; x < 1920; ++x) {
        const auto p = (y * 1920 + x) * 3;
        source[p] = x % 8 == 2 ? 240 : 0;
        source[p + 1] = y % 5 == 2 ? 200 : 0;
        source[p + 2] = 73;
    }
    std::vector<std::uint8_t> output(720 * 576 * 3);
    QVERIFY(FrameProcessor::resizeBgr(source,1920,1440,output,720,576,ScalingAlgorithm::Area));
    const std::array<int,3> blue{60,30,0};
    for (int y = 0; y < 576; ++y) for (int x = 0; x < 720; ++x) {
        const auto p = (y * 720 + x) * 3;
        QCOMPARE(output[p],blue[x % 3]);
        QCOMPARE(output[p+1],40);
        QCOMPARE(output[p+2],73);
    }
}

void ScalerTest::invalidInputs()
{
    std::array<std::uint8_t,36> source{}, destination{};
    const SourceFrame frame{source.data(),4,3,12,PixelFormat::Bgr};
    ScalePlan plan;
    const ScalingGeometry geometry{0,0,4,3,Rotation::None,4,3};
    QVERIFY(prepareScalePlan(frame,geometry,ScalingAlgorithm::Nearest,plan));
    QVERIFY(!scaleRows(plan,destination,2,1));
    QVERIFY(!scaleRows(plan,destination,0,4));
    QVERIFY(!scaleRows(plan,std::span(destination).first(35),0,3));
    QVERIFY(!prepareScalePlan(frame,geometry,static_cast<ScalingAlgorithm>(99),plan));
    QVERIFY(!scaleRows(plan,destination,0,3));
    QVERIFY(!FrameProcessor::convertToBgr({nullptr,4,3,12,PixelFormat::Bgr},destination,4,3));
    QVERIFY(!FrameProcessor::convertToBgr({source.data(),4,3,11,PixelFormat::Bgr},destination,4,3));
    QVERIFY(!FrameProcessor::convertToBgr({source.data(),0,3,0,PixelFormat::Bgr},destination,4,3));
    QVERIFY(!FrameProcessor::resizeBgr(source,4,3,destination,721,3));
    QVERIFY(!FrameProcessor::resizeBgr(std::span(source).first(35),4,3,destination,4,3));
}

QTEST_MAIN(ScalerTest)
#include "ScalerTest.moc"
