#include "core/CrtGeometry.h"
#include "core/FrameProcessor.h"
#include "core/ScalingExecutor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace mistercast;

namespace {
using Clock = std::chrono::steady_clock;

double milliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

struct Samples {
    std::vector<double> geometry, total;
    double cpuMs{};
    std::uint64_t checksum{};
};

void statistics(std::vector<double>& times)
{
    std::sort(times.begin(), times.end());
    for (const auto percentile : {0.50, 0.95, 0.99, 1.00}) {
        const auto index = static_cast<std::size_t>(std::ceil(percentile * times.size())) - 1;
        std::cout << ',' << times[index];
    }
}

Samples run(ScalingExecutor& executor, const Modeline& mode, ScalingAlgorithm scaling,
            CrtGeometrySettings settings, std::span<const std::uint8_t> source,
            std::size_t iterations)
{
    const auto plan = CrtGeometryPlan::make(mode, settings);
    if (!plan) throw std::runtime_error("invalid geometry benchmark");
    std::vector<std::uint8_t> baseline(mode.fullFrameBytes()), transformed(baseline.size());
    Samples samples;
    samples.geometry.reserve(iterations);
    samples.total.reserve(iterations);
    auto cpuStarted = std::clock();
    for (std::size_t i = 0; i < iterations + 12; ++i) {
        if (i == 12) cpuStarted = std::clock();
        const auto started = Clock::now();
        if (!FrameProcessor::resizeBgr(executor, source, 1920, 1440, baseline,
                                        mode.hActive, mode.vActive, scaling)) {
            throw std::runtime_error("scale failed");
        }
        const auto afterScale = Clock::now();
        if (settings.effective() && !executor.applyGeometry(*plan, baseline, transformed)) {
            throw std::runtime_error("geometry failed");
        }
        const auto finished = Clock::now();
        if (i >= 12) {
            samples.geometry.push_back(settings.effective() ? milliseconds(finished - afterScale) : 0);
            samples.total.push_back(milliseconds(finished - started));
        }
    }
    samples.cpuMs = 1e3 * (std::clock() - cpuStarted) / CLOCKS_PER_SEC / iterations;
    for (const auto value : settings.effective() ? transformed : baseline) {
        samples.checksum = samples.checksum * 31 + value;
    }
    return samples;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto workers = argc > 1 ? std::stoul(argv[1]) : ScalingExecutor::defaultWorkerCount();
        const auto iterations = argc > 2 ? std::stoul(argv[2]) : 30;
        if (!workers || !iterations) throw std::invalid_argument("arguments must be positive");
        const std::string filter = argc > 3 ? argv[3] : "";
        const std::string scalerFilter = argc > 4 ? argv[4] : "";
        std::vector<std::uint8_t> source(1920 * 1440 * 3);
        for (std::size_t i = 0; i < source.size(); ++i) source[i] = (i*71+i/37)%256;
        std::cout << "1920x1440 BGR; hardware_concurrency=" << std::thread::hardware_concurrency()
                  << "; iterations=" << iterations << "; geometry_buffer_bytes=" << kMaximumFrameBytes
                  << "; plan_bytes=" << sizeof(CrtGeometryPlan) << '\n';
        std::cout << "raster,cadence_hz,scaler,workers,geometry,plan_us,"
                  << "geometry_p50_ms,geometry_p95_ms,geometry_p99_ms,geometry_max_ms,"
                  << "total_p50_ms,total_p95_ms,total_p99_ms,total_max_ms,cpu_ms_per_frame,checksum\n";

        const struct { const char* name; int width, height, hz; bool interlaced; } modes[]{
            {"320x240p", 320, 240, 60, false}, {"384x288p", 384, 288, 50, false},
            {"640x480i", 640, 480, 60, true}, {"720x576i", 720, 576, 50, true},
            {"720x576p", 720, 576, 60, false}
        };
        const struct { const char* name; CrtGeometrySettings settings; } variants[]{
            {"off", {}}, {"neutral", {true}}, {"horizontal", {true, 7, -9}},
            {"horizontal-cabinet-off", {true, 7, -9, 0, 7}},
            {"cabinet-lut", {true, 0, 0, 0, 7, true}}, {"combined", {true, 7, -9, -5, 7, true}}
        };
        const std::vector<std::size_t> counts = workers == 1 ? std::vector<std::size_t>{1}
            : std::vector<std::size_t>{1, workers};
        for (const auto count : counts) {
            ScalingExecutor executor(count);
            for (const auto& raster : modes) {
                if (!filter.empty() && filter != raster.name) continue;
                Modeline mode{.name = {}, .hActive = static_cast<std::uint16_t>(raster.width),
                    .vActive = static_cast<std::uint16_t>(raster.height), .interlaced = raster.interlaced};
                for (const auto& descriptor : availableScalers()) {
                    if (!scalerFilter.empty() && scalerFilter != descriptor.key) continue;
                    if (raster.height < 576 && descriptor.algorithm != ScalingAlgorithm::Nearest) continue;
                    for (const auto& variant : variants) {
                        const auto start = Clock::now();
                        const auto plan = CrtGeometryPlan::make(mode, variant.settings);
                        const double planUs = milliseconds(Clock::now() - start) * 1000;
                        if (!plan) throw std::runtime_error("invalid plan");
                        auto samples = run(executor, mode, descriptor.algorithm, variant.settings,
                                           source, iterations);
                        std::cout << raster.name << ',' << raster.hz << ',' << descriptor.key << ','
                                  << count << ',' << variant.name << ',' << std::fixed
                                  << std::setprecision(3) << planUs;
                        statistics(samples.geometry);
                        statistics(samples.total);
                        std::cout << ',' << samples.cpuMs << ',' << samples.checksum << std::endl;
                    }
                }
            }
            if (executor.threadsStarted() != count) throw std::runtime_error("worker count changed");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
