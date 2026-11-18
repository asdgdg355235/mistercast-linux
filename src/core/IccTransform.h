#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace mistercast {

class IccTransform;

struct IccProfileLoad {
    std::shared_ptr<const IccTransform> transform;
    std::string error;
};

// The compiled transform and calibration tables can be shared by processing
// snapshots. LittleCMS's mutable single-pixel cache is disabled.
class IccTransform final {
public:
    static IccProfileLoad load(const std::filesystem::path& path);
    ~IccTransform();

    [[nodiscard]] bool hasCalibration() const noexcept;
    [[nodiscard]] bool malformedCalibration() const noexcept;
    [[nodiscard]] bool calibrationBakedIn() const noexcept;
    [[nodiscard]] static bool validBuffers(std::span<const std::uint8_t> source,
                                           std::span<std::uint8_t> destination,
                                           std::uint16_t width, std::uint16_t height) noexcept;
    [[nodiscard]] bool apply(std::span<const std::uint8_t> source,
                             std::span<std::uint8_t> destination,
                             std::uint16_t width, std::uint16_t height,
                             bool applyCalibration) const noexcept;

private:
    struct Data;
    explicit IccTransform(std::unique_ptr<Data> data);
    std::unique_ptr<Data> data_;
};

struct IccProcessingSettings {
    std::shared_ptr<const IccTransform> transform;
    bool applyCalibration{};
    bool operator==(const IccProcessingSettings&) const = default;
};

} // namespace mistercast
