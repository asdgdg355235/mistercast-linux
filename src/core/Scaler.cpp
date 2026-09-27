#include "core/Scaler.h"
#include "core/FrameProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mistercast {
namespace {
using detail::AxisSample;

AxisSample nearest(std::uint32_t d, std::uint32_t source, std::uint32_t output)
{
    const auto index = static_cast<std::uint32_t>(std::uint64_t{d} * source / output);
    return {index, index, 1, 0, 0, 1};
}

AxisSample bilinear(std::uint32_t d, std::uint32_t source, std::uint32_t output)
{
    // Pixel-center convention: (d + 0.5) * source / output - 0.5.
    // Keep integer numerators until accumulation, including edge clamping.
    const std::int64_t denominator = 2 * output;
    const auto numerator = std::clamp<std::int64_t>(
        (2 * std::int64_t{d} + 1) * source - output,
        0, std::int64_t{source - 1} * denominator);
    const auto first = static_cast<std::uint32_t>(numerator / denominator);
    const auto fraction = numerator % denominator;
    if (fraction == 0) {
        return {first, first, 1, 0, 0, 1};
    }
    return {first, first + 1, static_cast<double>(denominator - fraction),
            static_cast<double>(fraction), 0, static_cast<double>(denominator)};
}

AxisSample area(std::uint32_t d, std::uint32_t source, std::uint32_t output)
{
    if (source <= output) {
        return bilinear(d, source, output);
    }
    // Footprint [d*source/output, (d+1)*source/output). Integer endpoints
    // measured in 1/output units preserve fractional edge coverage exactly.
    const auto start = std::uint64_t{d} * source;
    const auto end = std::uint64_t{d + 1} * source;
    const auto first = static_cast<std::uint32_t>(start / output);
    const auto last = static_cast<std::uint32_t>((end - 1) / output);
    return {first, last, static_cast<double>(std::uint64_t{first + 1} * output - start),
            static_cast<double>(end - std::uint64_t{last} * output),
            static_cast<double>(output), static_cast<double>(source)};
}

const std::uint8_t* pixel(const ScalePlan& plan, std::uint32_t x, std::uint32_t y)
{
    return plan.source + std::int64_t{x} * plan.xStep + std::int64_t{y} * plan.yStep;
}

void nearestRows(const ScalePlan& plan, std::span<std::uint8_t> destination,
                 std::uint32_t firstRow, std::uint32_t endRow)
{
    const auto width = plan.geometry.outputWidth;
    for (auto y = firstRow; y < endRow; ++y) {
        auto* row = destination.data() + std::size_t{y} * width * 3;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto* p = pixel(plan, plan.x[x].first, plan.y[y].first);
            row[x * 3] = p[plan.blue];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[plan.red];
        }
    }
}

double weight(const AxisSample& sample, std::uint32_t index)
{
    if (index == sample.first) return sample.firstWeight;
    if (index == sample.last) return sample.lastWeight;
    return sample.interiorWeight;
}

void weightedRows(const ScalePlan& plan, std::span<std::uint8_t> destination,
                  std::uint32_t firstRow, std::uint32_t endRow)
{
    const auto width = plan.geometry.outputWidth;
    for (auto y = firstRow; y < endRow; ++y) {
        auto* row = destination.data() + std::size_t{y} * width * 3;
        const auto& ys = plan.y[y];
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto& xs = plan.x[x];
            double b = 0, g = 0, r = 0;
            for (auto sy = ys.first; sy <= ys.last; ++sy) {
                const auto wy = weight(ys, sy);
                const auto* p = pixel(plan, xs.first, sy);
                for (auto sx = xs.first; sx <= xs.last; ++sx) {
                    const auto w = weight(xs, sx) * wy;
                    b += p[plan.blue] * w;
                    g += p[1] * w;
                    r += p[plan.red] * w;
                    if (sx != xs.last) p += plan.xStep;
                }
            }
            const auto denominator = xs.totalWeight * ys.totalWeight;
            const auto rounded = [denominator](double value) {
                return static_cast<std::uint8_t>(
                    std::clamp(std::floor(value / denominator + 0.5), 0.0, 255.0));
            };
            row[x * 3] = rounded(b);
            row[x * 3 + 1] = rounded(g);
            row[x * 3 + 2] = rounded(r);
        }
    }
}

using AxisBuilder = AxisSample (*)(std::uint32_t, std::uint32_t, std::uint32_t);
struct Registration {
    ScalerDescriptor descriptor;
    AxisBuilder axis;
    ScalePlan::Kernel rows;
};
// The only registration list. UI descriptors are derived from these entries.
constexpr std::array registrations{
    Registration{{ScalingAlgorithm::Nearest, "nearest", "Nearest"}, nearest, nearestRows},
    Registration{{ScalingAlgorithm::Bilinear, "bilinear", "Bilinear"}, bilinear, weightedRows},
    Registration{{ScalingAlgorithm::Area, "area", "Area"}, area, weightedRows},
};
constexpr auto descriptors = [] {
    std::array<ScalerDescriptor, registrations.size()> result{};
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = registrations[i].descriptor;
    return result;
}();
}

std::span<const ScalerDescriptor> availableScalers() { return descriptors; }

const ScalerDescriptor& scalerDescriptor(ScalingAlgorithm algorithm)
{
    for (const auto& descriptor : descriptors) {
        if (descriptor.algorithm == algorithm) return descriptor;
    }
    return descriptors.front();
}

std::optional<ScalingAlgorithm> scalingAlgorithmFromKey(std::string_view key)
{
    for (const auto& descriptor : descriptors) {
        if (descriptor.key == key) return descriptor.algorithm;
    }
    return std::nullopt;
}

bool prepareScalePlan(const SourceFrame& source, const ScalingGeometry& geometry,
                      ScalingAlgorithm algorithm, ScalePlan& plan)
{
    plan.kernel = nullptr;
    const Registration* registration = nullptr;
    for (const auto& entry : registrations) {
        if (entry.descriptor.algorithm == algorithm) registration = &entry;
    }
    std::uint32_t bpp = 0;
    switch (source.format) {
    case PixelFormat::Bgr: case PixelFormat::Rgb: bpp = 3; break;
    case PixelFormat::Bgrx: case PixelFormat::Bgra:
    case PixelFormat::Rgbx: case PixelFormat::Rgba: bpp = 4; break;
    }
    if (!registration || !source.data || !bpp || !source.width || !source.height ||
        !geometry.cropWidth || !geometry.cropHeight ||
        !geometry.outputWidth || !geometry.outputHeight ||
        geometry.outputWidth > kMaximumActiveWidth || geometry.outputHeight > kMaximumActiveHeight ||
        geometry.cropWidth > source.width || geometry.cropHeight > source.height ||
        geometry.cropX > source.width - geometry.cropWidth ||
        geometry.cropY > source.height - geometry.cropHeight) return false;

    const auto rowBytes = std::int64_t{source.width} * bpp;
    const auto stride = source.stride == 0 ? rowBytes : source.stride;
    const auto absStride = stride < 0 ? -stride : stride;
    if (absStride < rowBytes ||
        std::uint64_t{source.height - 1} >
            (std::numeric_limits<std::ptrdiff_t>::max() - rowBytes) /
                static_cast<std::uint64_t>(absStride)) return false;

    bool quarterTurn = false, reverseX = false, reverseY = false;
    switch (geometry.rotation) {
    case Rotation::None: break;
    case Rotation::Clockwise90: quarterTurn = true; reverseX = true; break;
    case Rotation::CounterClockwise90: quarterTurn = true; reverseY = true; break;
    case Rotation::Flip180: reverseX = true; reverseY = true; break;
    default: return false;
    }
    plan.geometry = geometry;
    plan.source = source.data + std::int64_t{geometry.cropY} * stride +
        std::int64_t{geometry.cropX} * bpp;
    plan.xStep = quarterTurn ? stride : bpp;
    plan.yStep = quarterTurn ? bpp : stride;
    const bool rgb = source.format == PixelFormat::Rgb || source.format == PixelFormat::Rgbx ||
        source.format == PixelFormat::Rgba;
    plan.blue = rgb ? 2 : 0;
    plan.red = rgb ? 0 : 2;
    plan.copyRows = geometry.rotation == Rotation::None && source.format == PixelFormat::Bgr &&
        geometry.cropWidth == geometry.outputWidth && geometry.cropHeight == geometry.outputHeight;
    // Reverse destination coordinates before sampling, preserving legacy
    // nearest rounding for noninteger ratios in rotated frames.
    for (std::uint32_t x = 0; x < geometry.outputWidth; ++x) {
        plan.x[x] = registration->axis(reverseX ? geometry.outputWidth - x - 1 : x,
            quarterTurn ? geometry.cropHeight : geometry.cropWidth, geometry.outputWidth);
    }
    for (std::uint32_t y = 0; y < geometry.outputHeight; ++y) {
        plan.y[y] = registration->axis(reverseY ? geometry.outputHeight - y - 1 : y,
            quarterTurn ? geometry.cropWidth : geometry.cropHeight, geometry.outputHeight);
    }
    plan.kernel = registration->rows;
    return true;
}

bool scaleRows(const ScalePlan& plan, std::span<std::uint8_t> destination,
               std::uint32_t firstRow, std::uint32_t endRow)
{
    const auto rowBytes = std::size_t{plan.geometry.outputWidth} * 3;
    if (!plan.kernel || firstRow > endRow || endRow > plan.geometry.outputHeight ||
        destination.size() < rowBytes * plan.geometry.outputHeight) return false;
    if (plan.copyRows) {
        for (auto y = firstRow; y < endRow; ++y) {
            std::copy_n(pixel(plan, 0, y), rowBytes, destination.data() + y * rowBytes);
        }
    } else {
        plan.kernel(plan, destination, firstRow, endRow);
    }
    return true;
}
} // namespace mistercast
