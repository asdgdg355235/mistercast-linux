#pragma once

#include "core/Scaler.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace mistercast {

enum class PixelFormat {
    Bgr,
    Bgrx,
    Bgra,
    Rgb,
    Rgbx,
    Rgba,
};

struct SourceFrame {
    const std::uint8_t* data{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::int32_t stride{};
    PixelFormat format{PixelFormat::Bgrx};
};

enum class HorizontalAlignment {
    Left,
    Center,
    Right,
};

enum class VerticalAlignment {
    Top,
    Center,
    Bottom,
};

enum class Rotation {
    None,
    Clockwise90,
    CounterClockwise90,
    Flip180,
};

struct FramingSettings {
    HorizontalAlignment horizontal{HorizontalAlignment::Center};
    VerticalAlignment vertical{VerticalAlignment::Center};
    std::int32_t offsetX{};
    std::int32_t offsetY{};
    Rotation rotation{Rotation::None};
    ScalingAlgorithm scaling{ScalingAlgorithm::Nearest};
};

class FrameProcessor {
public:
    static bool convertToBgr(
        const SourceFrame& source,
        std::span<std::uint8_t> destination,
        std::uint16_t outputWidth,
        std::uint16_t outputHeight,
        const FramingSettings& framing = {});
    static bool resizeBgr(
        std::span<const std::uint8_t> source,
        std::uint16_t sourceWidth,
        std::uint16_t sourceHeight,
        std::span<std::uint8_t> destination,
        std::uint16_t outputWidth,
        std::uint16_t outputHeight,
        ScalingAlgorithm scaling = ScalingAlgorithm::Nearest);

    static bool prepareConversion(
        const SourceFrame& source, std::uint16_t outputWidth,
        std::uint16_t outputHeight, const FramingSettings& framing, ScalePlan& plan);
};

} // namespace mistercast
