#include "core/IccTransform.h"
#include "core/ScalingExecutor.h"
#include "IccFixtures.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <vector>

namespace mistercast {

class IccTransformTest : public QObject {
    Q_OBJECT

private slots:
    void profileErrors();
    void matrixAndLut();
    void bakedCalibration();
    void suppliedProfile();

private:
    static void compare(const std::filesystem::path& path, bool calibration);
};

void IccTransformTest::profileErrors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path base(dir.path().toStdString());
    QVERIFY(!IccTransform::load("relative.icc").transform);
    QVERIFY(!IccTransform::load(base / "missing.icc").transform);
    QVERIFY(!IccTransform::load(base).transform);
    const auto corrupt = base / "bad.icc";
    QFile file(QString::fromStdString(corrupt.string()));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(QByteArray(256, '\0')), 256);
    file.close();
    QVERIFY(!IccTransform::load(corrupt).transform);
    QVERIFY(icc_test::writeDisplay(corrupt, false, false));
    const auto originalSize = file.size();
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.resize(originalSize - 8));
    file.close();
    QVERIFY(!IccTransform::load(corrupt).transform);
    {
        icc_test::Profile link(cmsCreateNULLProfile(), cmsCloseProfile);
        QVERIFY(cmsSaveProfileToFile(link.get(), corrupt.c_str()));
        QVERIFY(!IccTransform::load(corrupt).transform);
    }
    QVERIFY(icc_test::writeDisplay(corrupt, false, false));
    QVERIFY(file.setPermissions(QFileDevice::Permissions{}));
    // Root can read mode 000. Exercise the denial when the OS enforces it.
    if (!file.open(QIODevice::ReadOnly)) QVERIFY(!IccTransform::load(corrupt).transform);
}

void IccTransformTest::compare(const std::filesystem::path& path, bool calibration)
{
    const auto result = IccTransform::load(path);
    QVERIFY2(result.transform, result.error.c_str());
    QCOMPARE(result.transform->hasCalibration(), calibration);
    icc_test::Profile input(cmsCreate_sRGBProfile(), cmsCloseProfile);
    icc_test::Profile output(cmsOpenProfileFromFile(path.c_str(), "r"), cmsCloseProfile);
    QVERIFY(output);
    // Independent reference: RGB formatter and the standard LittleCMS cache.
    std::unique_ptr<void, decltype(&cmsDeleteTransform)> reference(
        cmsCreateTransform(input.get(), TYPE_RGB_8, output.get(), TYPE_RGB_8,
                           INTENT_RELATIVE_COLORIMETRIC, 0), cmsDeleteTransform);
    QVERIFY(reference);
    std::vector<std::uint8_t> rgb;
    for (int r = 0; r <= 255; r += 17)
        for (int g = 0; g <= 255; g += 17)
            for (int b = 0; b <= 255; b += 17)
                rgb.insert(rgb.end(), {std::uint8_t(r), std::uint8_t(g), std::uint8_t(b)});
    for (std::uint8_t i = 0; i < 16; ++i) rgb.insert(rgb.end(), {i, i, i});

    constexpr std::uint16_t width = 257, height = 16;
    const auto pixels = width * height;
    QCOMPARE(rgb.size(), std::size_t{pixels * 3});
    auto bgr = rgb;
    std::vector<std::uint8_t> expected(rgb.size()), actual(rgb.size()), calibrated(rgb.size());
    cmsDoTransform(reference.get(), rgb.data(), expected.data(), pixels);
    for (std::size_t i = 0; i < rgb.size(); i += 3) {
        std::swap(bgr[i], bgr[i+2]);
        std::swap(expected[i], expected[i+2]);
    }
    QVERIFY(result.transform->apply(bgr, actual, width, height, false));
    QCOMPARE(actual, expected);
    QVERIFY(actual != bgr);
    QVERIFY(result.transform->apply(bgr, calibrated, width, height, true));
    if (calibration) {
        const auto* curves = static_cast<cmsToneCurve**>(cmsReadTag(output.get(), cmsSigVcgtTag));
        QVERIFY(curves);
        auto once = expected;
        auto twice = expected;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto quantize = [&](std::uint8_t value) {
                return std::uint8_t(std::lround(255 * std::clamp(
                    cmsEvalToneCurveFloat(curves[2 - i % 3], value / 255.0f), 0.0f, 1.0f)));
            };
            once[i] = quantize(expected[i]);
            twice[i] = quantize(once[i]);
        }
        QCOMPARE(calibrated, once);
        QVERIFY(calibrated != expected);
        QVERIFY(calibrated != twice);
    } else {
        QCOMPARE(calibrated, expected);
    }

    ScalingExecutor executor(4);
    std::vector<std::uint8_t> parallel(rgb.size());
    QVERIFY(executor.applyIcc(*result.transform, bgr, parallel, width, height, true));
    QCOMPARE(parallel, calibrated);
    QCOMPARE(executor.threadsStarted(), std::size_t{4});
    executor.shutdown();
    QVERIFY(!executor.applyIcc(*result.transform, bgr, parallel, width, height, true));

    QVERIFY(!result.transform->apply(bgr, actual, 0, 1, false));
    QVERIFY(!result.transform->apply(bgr, actual, 65535, 65535, false));
    QVERIFY(!result.transform->apply(std::span(bgr).subspan(1), actual, width, height, false));
    QVERIFY(!result.transform->apply(bgr, std::span(actual).subspan(1), width, height, false));
    QVERIFY(!result.transform->apply(bgr, bgr, width, height, false));

    auto second = actual;
    std::jthread concurrent([owned = result.transform, &bgr, &second] {
        if (!owned->apply(bgr, second, width, height, false)) std::abort();
    });
    QVERIFY(result.transform->apply(bgr, actual, width, height, false));
    concurrent.join();
    QCOMPARE(second, expected);
    QCOMPARE(actual, expected);
}

void IccTransformTest::matrixAndLut()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path base(dir.path().toStdString());
    const auto matrix = base / "Matrix 測定 #1.icm";
    const auto lut = base / "LUT + matrix.icc";
    QVERIFY(icc_test::writeDisplay(matrix, false, false));
    QVERIFY(icc_test::writeDisplay(lut, true, true));
    compare(matrix, false);
    compare(lut, true);

    // The profiles share matrix/TRCs. Relative colorimetric must select B2A1.
    auto matrixTransform = IccTransform::load(matrix).transform;
    auto lutTransform = IccTransform::load(lut).transform;
    QVERIFY(matrixTransform && lutTransform);
    const std::array<std::uint8_t, 3> black{};
    std::array<std::uint8_t, 3> matrixBlack{}, lutBlack{};
    QVERIFY(matrixTransform->apply(black, matrixBlack, 1, 1, false));
    QVERIFY(lutTransform->apply(black, lutBlack, 1, 1, false));
    QCOMPARE(matrixBlack, black);
    QVERIFY(lutBlack[0] > lutBlack[1] && lutBlack[1] > lutBlack[2] && lutBlack[2] > 0);
}

void IccTransformTest::bakedCalibration()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto base = std::filesystem::path(dir.path().toStdString());
    const auto original = base / "calibration.icc";
    const auto baked = base / "baked.icc";
    QVERIFY(icc_test::writeDisplay(original, true, true));
    icc_test::Profile p(cmsOpenProfileFromFile(original.c_str(), "r"), cmsCloseProfile);
    QVERIFY(p);
    const auto size = cmsReadRawTag(p.get(), cmsSigProfileDescriptionTag, nullptr, 0);
    QVERIFY(size > 0);
    std::vector<std::uint8_t> raw(size);
    QCOMPARE(cmsReadRawTag(p.get(), cmsSigProfileDescriptionTag, raw.data(), size), size);
    QVERIFY(cmsWriteRawTag(p.get(), static_cast<cmsTagSignature>(0x41523054), raw.data(), size));
    QVERIFY(cmsSaveProfileToFile(p.get(), baked.c_str()));

    const auto profile = IccTransform::load(baked);
    QVERIFY2(profile.transform, profile.error.c_str());
    QVERIFY(profile.transform->calibrationBakedIn());
    QVERIFY(!profile.transform->hasCalibration());
    const std::array<std::uint8_t, 3> input{80, 100, 120};
    std::array<std::uint8_t, 3> uncalibrated{}, calibrated{};
    QVERIFY(profile.transform->apply(input, uncalibrated, 1, 1, false));
    QVERIFY(profile.transform->apply(input, calibrated, 1, 1, true));
    QCOMPARE(calibrated, uncalibrated);
}

void IccTransformTest::suppliedProfile()
{
    const auto profile = qgetenv("MISTERCAST_ICC_TEST_PROFILE");
    if (profile.isEmpty()) QSKIP("Configure MISTERCAST_ICC_TEST_PROFILE to exercise an external display profile");
    const auto path = std::filesystem::path(profile.constData());
    icc_test::Profile p(cmsOpenProfileFromFile(path.c_str(), "r"), cmsCloseProfile);
    QVERIFY(p);
    QCOMPARE(cmsGetColorSpace(p.get()), cmsSigRgbData);
    QVERIFY(cmsIsTag(p.get(), cmsSigBToA1Tag));
    compare(path, cmsIsTag(p.get(), cmsSigVcgtTag));
}

} // namespace mistercast

QTEST_GUILESS_MAIN(mistercast::IccTransformTest)
#include "IccTransformTest.moc"
