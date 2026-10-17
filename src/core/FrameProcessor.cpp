#include "core/FrameProcessor.h"
#include "core/ScalingExecutor.h"

#include <algorithm>

namespace mistercast {

ScalingGeometry FrameProcessor::framingGeometry(
    const SourceFrame& source, std::uint16_t outputWidth,
    std::uint16_t outputHeight, const FramingSettings& framing)
{
    const bool quarterTurn = framing.rotation == Rotation::Clockwise90 ||
        framing.rotation == Rotation::CounterClockwise90;
    const std::uint32_t aspectWidth = quarterTurn ? 3 : 4;
    const std::uint32_t aspectHeight = quarterTurn ? 4 : 3;

    std::uint32_t cropWidth = source.width;
    std::uint32_t cropHeight = source.height;

    if (static_cast<std::uint64_t>(source.width) * aspectHeight >
        static_cast<std::uint64_t>(source.height) * aspectWidth) {
        cropWidth = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(source.height) * aspectWidth / aspectHeight);
    } else {
        cropHeight = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(source.width) * aspectHeight / aspectWidth);
    }

    const auto horizontalSpace = static_cast<std::int64_t>(source.width - cropWidth);
    const auto verticalSpace = static_cast<std::int64_t>(source.height - cropHeight);

    std::int64_t cropX = 0;
    
    switch (framing.horizontal) {
    case HorizontalAlignment::Left:
        break;
    case HorizontalAlignment::Center:
        cropX = horizontalSpace / 2;
        break;
    case HorizontalAlignment::Right:
        cropX = horizontalSpace;
        break;
    }

    std::int64_t cropY = 0;
    
    switch (framing.vertical) {
    case VerticalAlignment::Top:
        break;
    case VerticalAlignment::Center:
        cropY = verticalSpace / 2;
        break;
    case VerticalAlignment::Bottom:
        cropY = verticalSpace;
        break;
    }

    cropX = std::clamp(cropX + framing.offsetX, std::int64_t{0}, horizontalSpace);
    cropY = std::clamp(cropY + framing.offsetY, std::int64_t{0}, verticalSpace);

    return {static_cast<std::uint32_t>(cropX), static_cast<std::uint32_t>(cropY),
            cropWidth, cropHeight, framing.rotation, outputWidth, outputHeight};
}

bool FrameProcessor::prepareConversion(
    const SourceFrame& source, std::uint16_t outputWidth,
    std::uint16_t outputHeight, const FramingSettings& framing, ScalePlan& plan)
{
    return prepareScalePlan(source, framingGeometry(source, outputWidth, outputHeight, framing),
                            framing.scaling, plan);
}

bool FrameProcessor::convertToBgr(
    ScalingExecutor& executor,
    const SourceFrame& source, std::span<std::uint8_t> destination,
    std::uint16_t outputWidth, std::uint16_t outputHeight,
    const FramingSettings& framing)
{
    return executor.scale(source, framingGeometry(source, outputWidth, outputHeight, framing),
                          framing.scaling, destination);
}

bool FrameProcessor::resizeBgr(
    ScalingExecutor& executor,
    std::span<const std::uint8_t> source,
    std::uint16_t sourceWidth, std::uint16_t sourceHeight,
    std::span<std::uint8_t> destination,
    std::uint16_t outputWidth, std::uint16_t outputHeight,
    ScalingAlgorithm scaling)
{
    if (source.size() < static_cast<std::size_t>(sourceWidth) * sourceHeight * 3) {
        return false;
    }
    const SourceFrame frame{source.data(), sourceWidth, sourceHeight, 0, PixelFormat::Bgr};
    return executor.scale(frame,
        {0, 0, sourceWidth, sourceHeight, Rotation::None, outputWidth, outputHeight},
        scaling, destination);
}

} // namespace mistercast
