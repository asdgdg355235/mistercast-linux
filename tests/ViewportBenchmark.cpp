#include "core/FrameProcessor.h"
#include "core/ScalingExecutor.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace mistercast;

int main(int argc, char** argv)
{
    using Clock = std::chrono::steady_clock;
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 300;
    if (iterations < 10) return 2;
    ScalingExecutor executor;
    const Modeline raster{"Benchmark", 14.221170, 664, 692, 759, 873, 498, 501, 507, 543, true};
    std::vector<std::uint8_t> pixels(640 * 480 * 3, 111), rawCache(pixels.size());
    const SourceFrame source{pixels.data(), 640, 480, 640 * 3, PixelFormat::Bgr};
    std::vector<std::uint8_t> out(raster.fullFrameBytes()), scratch(kMaximumFrameBytes);
    std::cout << "case,median_us,p95_us,p99_us,max_us,workers\n";
    volatile unsigned observed = 0;
    for (int variant = 0; variant < 4; ++variant) {
        std::vector<double> samples(iterations);
        for (int i = -20; i < iterations; ++i) {
            const auto begin = Clock::now();
            const ContentViewport viewport{variant >= 2, true, variant == 3 ? 630 : 640,
                                            variant == 3 ? 472 : 480};
            if (!FrameProcessor::composeViewport(executor, source, out, scratch, raster, {}, viewport)) {
                return 1;
            }
            if (variant > 0) std::ranges::copy(pixels, rawCache.begin());
            const auto end = Clock::now();
            observed = out[0] + rawCache[std::max(i, 0) % rawCache.size()];
            if (i >= 0) samples[i] = std::chrono::duration<double, std::micro>(end - begin).count();
        }
        std::ranges::sort(samples);
        const auto p = [&](double q) { return samples[std::size_t(q * (samples.size() - 1))]; };
        constexpr const char* labels[]{"original", "full_raster_cached", "native_viewport_cached",
                                       "scaled_viewport_cached"};
        std::cout << labels[variant] << ',' << p(.50) << ',' << p(.95) << ',' << p(.99) << ','
                  << samples.back() << ',' << executor.workerCount() << '\n';
    }
    return observed < 256 ? 0 : 1;
}
