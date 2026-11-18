#include "IccFixtures.h"
#include "core/CrtGeometry.h"
#include "core/FrameProcessor.h"
#include "core/IccTransform.h"
#include "core/ScalingExecutor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <tuple>
#include <iostream>
#include <vector>

using namespace mistercast;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::cerr << "Usage: icc_benchmark /absolute/lut.icc /absolute/matrix-fixture.icm iterations\n";
        return 2;
    }
    const std::filesystem::path fixture(std::filesystem::absolute(argv[2]));
    if (!icc_test::writeDisplay(fixture, false, false)) return 1;
    auto matrix = IccTransform::load(fixture);
    auto lut = IccTransform::load(std::filesystem::absolute(argv[1]));
    if (!matrix.transform || !lut.transform || !lut.transform->hasCalibration()) {
        std::cerr << matrix.error << lut.error << " Supply a LUT profile with vcgt.\n";
        return 1;
    }

    const int iterations = std::max(std::atoi(argv[3]), 1);
    ScalingExecutor executor;
    std::vector<std::uint8_t> input(kMaximumFrameBytes);
    std::uint32_t state = 1;
    for (auto& byte : input) {
        state = state * 1664525 + 1013904223;
        byte = state >> 24;
    }
    std::cout << "width,height,scan,case,median_us,p95_us,p99_us,max_us,"
                 "icc_p99_us,icc_buffer_bytes,executor_workers\n";
    for (const auto& mode : std::array{Modeline{.name={},.hActive=320,.vActive=240},
            Modeline{.name={},.hActive=384,.vActive=288},
            Modeline{.name={},.hActive=640,.vActive=480,.interlaced=true},
            Modeline{.name={},.hActive=720,.vActive=576,.interlaced=true}}) {
        const auto w = mode.hActive, h = mode.vActive;
        const SourceFrame source{input.data(), w, h, w * 3, PixelFormat::Rgb};
        const auto plan = CrtGeometryPlan::make(mode, {true, 0, 0, 0, -3});
        if (!plan) return 1;
        for (const auto& [label, transform, calibration, cabinet] : std::array{
                 std::tuple{"off", static_cast<const IccTransform*>(nullptr), false, false},
                 std::tuple{"cabinet", static_cast<const IccTransform*>(nullptr), false, true},
                 std::tuple{"matrix", matrix.transform.get(), false, false},
                 std::tuple{"lut", lut.transform.get(), false, false},
                 std::tuple{"lut-vcgt", lut.transform.get(), true, false},
                 std::tuple{"lut-vcgt-cabinet", lut.transform.get(), true, true}}) {
            std::vector<std::uint8_t> converted(mode.fullFrameBytes()), geometry(cabinet ? mode.fullFrameBytes() : 0);
            std::vector<std::uint8_t> icc(transform ? kMaximumFrameBytes : 0);
            std::vector<double> totalTimes(iterations), iccTimes(iterations);
            for (int i = -10; i < iterations; ++i) {
                const auto start = Clock::now();
                if (!FrameProcessor::convertToBgr(executor, source, converted, w, h, {})) return 1;
                std::span<const std::uint8_t> output = converted;
                if (cabinet) {
                    if (!executor.applyGeometry(*plan, output, geometry)) return 1;
                    output = geometry;
                }
                const auto iccStart = Clock::now();
                if (transform && !executor.applyIcc(*transform, output, icc, w, h, calibration)) return 1;
                const auto end = Clock::now();
                if (i >= 0) {
                    totalTimes[i] = std::chrono::duration<double, std::micro>(end - start).count();
                    iccTimes[i] = transform ? std::chrono::duration<double, std::micro>(end - iccStart).count() : 0;
                }
            }
            std::sort(totalTimes.begin(), totalTimes.end());
            std::sort(iccTimes.begin(), iccTimes.end());
            const auto index = [&](double quantile) { return std::size_t(quantile * (iterations - 1)); };
            std::cout << w << ',' << h << ',' << (mode.interlaced ? "i" : "p") << ',' << label << ','
                      << totalTimes[index(.5)] << ',' << totalTimes[index(.95)] << ','
                      << totalTimes[index(.99)] << ',' << totalTimes.back() << ','
                      << iccTimes[index(.99)] << ',' << icc.size() << ',' << executor.threadsStarted() << '\n';
        }
    }
}
