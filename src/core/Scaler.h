#pragma once

#include "core/Modeline.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace mistercast {

enum class ScalingAlgorithm { Nearest, Bilinear, Area };

struct ScalerDescriptor {
    ScalingAlgorithm algorithm;
    std::string_view key;
    std::string_view displayName;
};

std::span<const ScalerDescriptor> availableScalers();
const ScalerDescriptor& scalerDescriptor(ScalingAlgorithm algorithm);
std::optional<ScalingAlgorithm> scalingAlgorithmFromKey(std::string_view key);

struct SourceFrame;
enum class Rotation;

struct ScalingGeometry {
    std::uint32_t cropX{}, cropY{}, cropWidth{}, cropHeight{};
    Rotation rotation{};
    std::uint16_t outputWidth{}, outputHeight{};
};

namespace detail {
// Integer coverage weights. Interior pixels share one weight, so storage is
// bounded by destination dimensions even for very large source footprints.
struct AxisSample {
    std::uint32_t first{}, last{};
    double firstWeight{}, lastWeight{}, interiorWeight{}, totalWeight{};
};
}

struct ScalePlan {
    ScalingGeometry geometry{};
    std::array<detail::AxisSample, kMaximumActiveWidth> x;
    std::array<detail::AxisSample, kMaximumActiveHeight> y;
    const std::uint8_t* source{};
    std::int64_t xStep{}, yStep{};
    std::uint32_t blue{}, red{};
    bool copyRows{};
    using Kernel = void (*)(const ScalePlan&, std::span<std::uint8_t>,
                            std::uint32_t, std::uint32_t);
    Kernel kernel{};
};

// SourceFrame::data addresses logical row zero, including for negative stride.
// The caller owns source storage and must keep it alive and unchanged. After
// preparation, share the plan read-only. Destination must not overlap source.
bool prepareScalePlan(const SourceFrame& source, const ScalingGeometry& geometry,
                      ScalingAlgorithm algorithm, ScalePlan& plan);

// Destination is the full output buffer. Only [firstRow,endRow) is written;
// disjoint row calls need no synchronization or execution ordering.
bool scaleRows(const ScalePlan& plan, std::span<std::uint8_t> destination,
               std::uint32_t firstRow, std::uint32_t endRow);

} // namespace mistercast
