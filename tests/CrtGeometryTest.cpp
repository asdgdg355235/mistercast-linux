#include "core/CrtGeometry.h"
#include "core/FrameFields.h"
#include "core/FrameProcessor.h"
#include "core/ScalingExecutor.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace mistercast;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ':' + std::to_string(__LINE__) + " " #condition); } while (false)

namespace {
std::atomic_uint64_t geometryAllocations{};
std::atomic_bool trackGeometryAllocations{};
}

// Linker wrapping counts allocations made by the executable and its static
// core library. Library-internal and operating-system allocations are outside
// this probe. Thread startup, setup and printing happen outside the window.
extern "C" void* __real__Znwm(std::size_t);
extern "C" void* __real__Znam(std::size_t);
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __real_calloc(std::size_t, std::size_t);
extern "C" void* __real_realloc(void*, std::size_t);

static void countGeometryAllocation()
{
    if (trackGeometryAllocations.load(std::memory_order_relaxed)) {
        geometryAllocations.fetch_add(1, std::memory_order_relaxed);
    }
}
extern "C" void* __wrap__Znwm(std::size_t size)
{
    countGeometryAllocation();
    return __real__Znwm(size);
}
extern "C" void* __wrap__Znam(std::size_t size)
{
    countGeometryAllocation();
    return __real__Znam(size);
}
extern "C" void* __wrap_malloc(std::size_t size)
{
    countGeometryAllocation();
    return __real_malloc(size);
}
extern "C" void* __wrap_calloc(std::size_t count, std::size_t size)
{
    countGeometryAllocation();
    return __real_calloc(count, size);
}
extern "C" void* __wrap_realloc(void* pointer, std::size_t size)
{
    countGeometryAllocation();
    return __real_realloc(pointer, size);
}

namespace {

Modeline raster(int width, int height, bool interlaced = false)
{
    auto mode = kDefaultModeline;
    mode.hActive = width;
    mode.vActive = height;
    mode.interlaced = interlaced;
    return mode;
}

void coordinates()
{
    const auto mode = raster(73, 240);
    for (int h = -16; h <= 15; ++h) {
        for (int dx = -48; dx <= 48; ++dx) {
            auto plan = CrtGeometryPlan::make(mode, {true, h, dx});
            CHECK(plan);
            for (int x = 0; x < mode.hActive; ++x) {
                const auto coordinate = (x - dx) * 64.0 / (64 + h);
                const int expected = coordinate >= 0 && coordinate < mode.hActive
                    ? static_cast<int>(coordinate) : -1;
                CHECK(plan->sourceColumn(x) == expected);
            }
        }
    }

    for (const int lanes : {1, 2}) {
        for (const int n : {1, 4, 224, 240, 288}) {
            const auto mode = raster(16, n * lanes, lanes == 2);
            for (int s = minimumCabinetSize(n); s <= 7; ++s) {
                for (int dy = -16; dy <= 15; ++dy) {
                    const auto plan = CrtGeometryPlan::make(mode, {true, 0, 0, dy, s});
                    CHECK(plan);
                    const int m = n + 3 * s;
                    int pos = 0;
                    for (int j = 0; j < m; ++j, pos += (n << 8) / m) {
                        const int y = j + std::max(n - m, 0) + dy;
                        if (y < 0 || y >= n) continue;
                        for (int p = 0; p < lanes; ++p) {
                            const auto row = plan->sourceRow(y * lanes + p);
                            CHECK(row.first == (pos >> 8) * lanes + p);
                            CHECK(row.second == std::min((pos >> 8) + 1, n - 1) * lanes + p);
                            CHECK(row.weight == (row.first != row.second ? pos & 255u : 0));
                        }
                    }
                }
            }
        }
    }
    CHECK(!CrtGeometryPlan::make(raster(8, 1), {true, 0, 0, 0, -1}));
    CHECK(!CrtGeometryPlan::make(raster(8, 3, true), {true, 1}));
    CHECK(!CrtGeometryPlan::make(raster(0, 3), {}));
    const auto b = crtGeometryBounds(raster(8, 4), {true, -16, -48, 15, 1});
    CHECK(b.left == 6 && b.right == 0 && b.top == 0 && b.bottom == 7);
}

void pixels()
{
    ScalingExecutor executor(4), single(1);
    for (const auto& mode : {raster(320, 240), raster(384, 288),
                            raster(640, 480, true), raster(720, 576, true)}) {
        std::vector<std::uint8_t> src(mode.fullFrameBytes()), dest(src.size()), expected(src.size()),
            field(mode.payloadBytes());
        const auto width = mode.hActive;
        for (unsigned y = 0; y < mode.vActive; ++y) {
            for (unsigned x = 0; x < width; ++x) {
                const auto offset = (y * width + x) * 3;
                src[offset] = (y & 1) ? 255 : 31;
                src[offset + 1] = x % 256;
                src[offset + 2] = y % 256;
            }
        }
        const auto a = *CrtGeometryPlan::make(mode, {true, 6, -5, -3, -2});
        const auto b = *CrtGeometryPlan::make(mode, {true, -16, 15, 1, 6});
        const auto neutral = *CrtGeometryPlan::make(mode, {});
        CHECK(!executor.applyGeometry(a, src, src));
        CHECK(!executor.applyGeometry(a, std::span(src).first(3), dest));
        CHECK(executor.applyGeometry(neutral, src, dest));
        CHECK(dest == src);
        CHECK(single.applyGeometry(a, src, expected));
        CHECK(executor.applyGeometry(b, src, dest));
        CHECK(executor.applyGeometry(a, src, dest));
        CHECK(dest == expected);
        for (unsigned y = 0; y < mode.vActive; ++y) {
            const auto row = a.sourceRow(y);
            for (unsigned x = 0; x < width; ++x) {
                const int col = a.sourceColumn(x);
                for (int c = 0; c < 3; ++c) {
                    const auto want = row.first < 0 || col < 0 ? 0 :
                        blendGamma24(src[(row.first * width + col) * 3 + c],
                                     src[(row.second * width + col) * 3 + c], row.weight);
                    CHECK(dest[(y * width + x) * 3 + c] == want);
                }
            }
        }
        if (mode.interlaced) {
            for (auto parity : {FieldParity::Field0OddSourceLines, FieldParity::Field1EvenSourceLines}) {
                CHECK(extractInterlacedField(dest, mode, parity, field));
                for (std::size_t i = 0; i < field.size(); i += 3) {
                    CHECK(field[i] == 0 || field[i] ==
                        (parity == FieldParity::Field0OddSourceLines ? 255 : 31));
                }
            }
        }
    }

    const auto mode = raster(32, 24, true);
    const auto plan = *CrtGeometryPlan::make(mode, {true, 15, -5, -1, 2});
    std::vector<std::uint8_t> input(mode.fullFrameBytes()), out(input.size()), want(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = (19 * i + i / 7) % 256;
    CHECK(single.applyGeometry(plan, input, want));
    trackGeometryAllocations = true;
    for (int i = 0; i < 1000; ++i) {
        CHECK(executor.applyGeometry(plan, input, out));
        CHECK(out == want);
    }
    trackGeometryAllocations = false;
    CHECK(geometryAllocations == 0);
    std::cout << "repeated_frames=1000 observed_geometry_allocations=" << geometryAllocations << '\n';
    for (int i = 0; i < 24; ++i) {
        // Move both the columns and the rows through the fixed sampling phase.
        std::rotate(input.begin(), input.begin() + 3 * (mode.hActive + 1), input.end());
        CHECK(single.applyGeometry(plan, input, want));
        CHECK(executor.applyGeometry(plan, input, out));
        CHECK(out == want);
    }
    CHECK(executor.threadsStarted() == 4);
    std::jthread rendering([&] { executor.applyGeometry(plan, input, out); });
    executor.shutdown();
    rendering.join();
    CHECK(!executor.applyGeometry(plan, input, out));
}

void viewportAllocations()
{
    const auto mode = raster(664, 498, true);
    ScalingExecutor executor(4);
    std::vector<std::uint8_t> source(640 * 480 * 3), destination(mode.fullFrameBytes());
    std::vector<std::uint8_t> scratch(kMaximumFrameBytes);
    const SourceFrame frame{source.data(), 640, 480, 640 * 3, PixelFormat::Bgr};
    const ContentViewport native{true, true, 640, 480};
    const ContentViewport scaled{true, true, 620, 465};
    const auto compose = [&](const ContentViewport& viewport) {
        return FrameProcessor::composeViewport(executor, frame, destination, scratch, mode, {}, viewport);
    };
    CHECK(compose(native));
    CHECK(compose(scaled));
    const auto before = geometryAllocations.load();
    trackGeometryAllocations = true;
    for (int i = 0; i < 500; ++i) {
        CHECK(compose(native));
        CHECK(compose(scaled));
    }
    trackGeometryAllocations = false;
    CHECK(geometryAllocations.load() == before);
    CHECK(executor.threadsStarted() == 4);
    std::cout << "repeated_viewport_frames=1000 observed_viewport_allocations=0\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 2) {
            std::ifstream trace(argv[1]);
            int n, s, y, first, weight;
            int samples = 0;
            char axis;
            while (trace >> axis >> n >> s >> y >> first >> weight) {
                CHECK(y >= 0 && y < n);
                if (axis == 'H') {
                    const auto plan = CrtGeometryPlan::make(raster(n, 8), {true, s});
                    CHECK(plan && plan->sourceColumn(y) == first);
                } else {
                    CHECK(axis == 'V');
                    const auto plan = CrtGeometryPlan::make(raster(8, n), {true, 0, 0, 0, s});
                    CHECK(plan);
                    const auto row = plan->sourceRow(y);
                    CHECK(row.first == first && row.second == std::min(first + 1, n - 1));
                    CHECK(row.weight == static_cast<unsigned>(weight));
                }
                ++samples;
            }
            CHECK(trace.eof() && samples > 1000);
            return 0;
        }
        coordinates();
        viewportAllocations();
        pixels();
        CHECK(blendGamma24(0, 255, 128) == 191);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
