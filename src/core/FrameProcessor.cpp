#include "core/FrameProcessor.h"
#include "core/ScalingExecutor.h"

#include <algorithm>

namespace mistercast {

ContentViewport clampContentViewport(ContentViewport viewport, const Modeline& raster)
{
    viewport.width = std::clamp(viewport.width ? viewport.width : int(raster.hActive),
                               1, int(kMaximumActiveWidth));
    viewport.height = std::clamp(viewport.height ? viewport.height : int(raster.vActive),
                                1, int(kMaximumActiveHeight));
    viewport.x = std::clamp(viewport.x, -int(kMaximumActiveWidth), int(raster.hActive));
    viewport.y = std::clamp(viewport.y, -int(kMaximumActiveHeight), int(raster.vActive));
    return viewport;
}

ContentRect contentRect(const ContentViewport& viewport, const Modeline& raster)
{
    if (!viewport.enabled) return {0, 0, raster.hActive, raster.vActive};
    const auto center = [](int delta) { return (delta - (delta < 0 && delta % 2 != 0)) / 2; };
    const auto v = clampContentViewport(viewport, raster);
    return {v.centered ? center(raster.hActive - v.width) : v.x,
            v.centered ? center(raster.vActive - v.height) : v.y, v.width, v.height};
}

ContentRect visibleContentRect(const ContentRect& content, const Modeline& raster)
{
    const auto x = std::clamp(content.x, 0, int(raster.hActive));
    const auto y = std::clamp(content.y, 0, int(raster.vActive));
    return {x, y, std::clamp(content.x + content.width, x, int(raster.hActive)) - x,
                  std::clamp(content.y + content.height, y, int(raster.vActive)) - y};
}

bool FrameProcessor::composeViewport(ScalingExecutor& executor, const SourceFrame& source,
    std::span<std::uint8_t> destination, std::span<std::uint8_t> scratch,
    const Modeline& raster, const FramingSettings& framing, const ContentViewport& viewport)
{
    if (!raster.hActive || !raster.vActive || raster.hActive > kMaximumActiveWidth ||
        raster.vActive > kMaximumActiveHeight || destination.size() < raster.fullFrameBytes()) return false;

    if (!viewport.enabled) {
        return convertToBgr(executor, source, destination, raster.hActive, raster.vActive, framing);
    }
    if (viewport.width < 1 || viewport.height < 1 ||
        viewport.width > kMaximumActiveWidth || viewport.height > kMaximumActiveHeight) return false;

    const auto content = contentRect(viewport, raster);
    const auto visible = visibleContentRect(content, raster);
    const auto crop = framingGeometry(source, content.width, content.height, framing);
    ScalingInput input;
    if (!prepareScalingInput(source, crop, input)) return false;

    auto output = destination.first(raster.fullFrameBytes());
    if (!visible.width || !visible.height) {
        std::ranges::fill(output, 0);
        return true;
    }
    if (content.x == 0 && content.y == 0 && content.width == raster.hActive &&
        content.height == raster.vActive) {
        return executor.scale(source, crop, framing.scaling, output);
    }

    // Place an already native BGR crop directly into the pitched raster.
    const auto contentStride = content.width * 3;
    if (source.format != PixelFormat::Bgr || framing.rotation != Rotation::None ||
        crop.cropWidth != crop.outputWidth || crop.cropHeight != crop.outputHeight) {
        const auto bytes = std::size_t(contentStride) * content.height;
        if (scratch.size() < bytes ||
            !executor.scale(source, crop, framing.scaling, scratch.first(bytes))) return false;
        input.pixels = scratch.data();
        input.stride = contentStride;
    }
    std::ranges::fill(output, 0);
    const auto stride = raster.hActive * 3;
    const auto* first = input.pixels +
        (visible.y - content.y) * input.stride + (visible.x - content.x) * 3;
    auto* destinationRow = output.data() + visible.y * stride + visible.x * 3;
    for (int y = 0; y < visible.height; ++y) {
        std::copy_n(first + y * input.stride, visible.width * 3, destinationRow + y * stride);
    }
    return true;
}

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
