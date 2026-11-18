#include "core/IccTransform.h"

#include "core/Modeline.h"

#include <lcms2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <fstream>
#include <optional>
#include <type_traits>
#include <vector>

namespace mistercast {
namespace {

using Profile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
using Calibration = std::array<std::array<std::uint8_t, 256>, 3>;
constexpr auto kMaximumProfileBytes = 64 * 1024 * 1024;

std::optional<Calibration> readCalibration(cmsHPROFILE profile)
{
    // LittleCMS decodes both the table and formula forms to three RGB curves.
    const auto* curves = static_cast<cmsToneCurve* const*>(cmsReadTag(profile, cmsSigVcgtTag));
    if (!curves) return std::nullopt;

    Calibration values{};
    for (std::size_t channel = 0; channel < values.size(); ++channel) {
        const auto* curve = curves[2 - channel]; // RGB ICC curves, packed BGR pixels.
        if (!curve) return std::nullopt;
        for (std::size_t code = 0; code < values[channel].size(); ++code) {
            const auto value = cmsEvalToneCurveFloat(curve, static_cast<float>(code) / 255);
            if (!std::isfinite(value)) return std::nullopt;
            values[channel][code] = std::lround(std::clamp(value, 0.0f, 1.0f) * 255);
        }
    }
    return values;
}

} // namespace

struct IccTransform::Data {
    std::unique_ptr<std::remove_pointer_t<cmsContext>, decltype(&cmsDeleteContext)>
        context{cmsCreateContext(nullptr, nullptr), cmsDeleteContext};
    std::unique_ptr<void, decltype(&cmsDeleteTransform)> transform{nullptr, cmsDeleteTransform};
    std::optional<Calibration> calibration;
    bool hasCalibrationTag{};
    bool calibrationBakedIn{};
};

IccTransform::IccTransform(std::unique_ptr<Data> data) : data_(std::move(data)) {}
IccTransform::~IccTransform() = default;

IccProfileLoad IccTransform::load(const std::filesystem::path& path)
{
    if (!path.is_absolute()) return {{}, "Enter an absolute ICC profile path."};
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return {{}, "The ICC path does not point to an accessible file."};
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {{}, "Cannot open the ICC profile for reading."};
    const auto length = file.tellg();
    if (length < 132 || length > kMaximumProfileBytes) {
        return {{}, "Invalid ICC profile size (limit 64 MiB)."};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), length)) {
        return {{}, "Cannot read the complete ICC profile."};
    }
    // The ICC envelope declares its size in the first big-endian word.
    // Reject truncation even if the remaining tags suffice for a transform.
    std::uint32_t declaredSize = 0;
    for (std::size_t i = 0; i < 4; ++i) declaredSize = (declaredSize << 8) | bytes[i];
    if (declaredSize < 132 || declaredSize > bytes.size()) {
        return {{}, "The ICC profile header is invalid or the file is truncated."};
    }

    auto data = std::make_unique<Data>();
    if (!data->context) return {{}, "Cannot initialize LittleCMS."};
    const Profile destination(cmsOpenProfileFromMemTHR(data->context.get(), bytes.data(), declaredSize),
                              cmsCloseProfile);
    if (!destination) return {{}, "LittleCMS could not parse the ICC profile."};
    if (cmsGetDeviceClass(destination.get()) != cmsSigDisplayClass ||
        cmsGetColorSpace(destination.get()) != cmsSigRgbData ||
        (cmsGetPCS(destination.get()) != cmsSigXYZData && cmsGetPCS(destination.get()) != cmsSigLabData)) {
        return {{}, "Select an RGB display ICC profile with an XYZ or Lab connection space."};
    }

    const Profile source(cmsCreate_sRGBProfileTHR(data->context.get()), cmsCloseProfile);
    if (source) {
        data->transform.reset(cmsCreateTransformTHR(data->context.get(), source.get(), TYPE_BGR_8,
            destination.get(), TYPE_BGR_8, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE));
    }
    if (!data->transform) return {{}, "LittleCMS could not build a transform to this display profile."};

    data->hasCalibrationTag = cmsIsTag(destination.get(), cmsSigVcgtTag);
    // ArgyllCMS 3.3 profile/applycal.c (-c) identifies baked calibration
    // by the private backup of the profile description (AR0T).
    constexpr auto argyllCalibrationBackup = static_cast<cmsTagSignature>(0x41523054);
    data->calibrationBakedIn = cmsIsTag(destination.get(), argyllCalibrationBackup);
    if (data->hasCalibrationTag && !data->calibrationBakedIn) {
        data->calibration = readCalibration(destination.get());
    }
    return {std::shared_ptr<const IccTransform>(new IccTransform(std::move(data))), {}};
}

bool IccTransform::hasCalibration() const noexcept { return data_->calibration.has_value(); }
bool IccTransform::malformedCalibration() const noexcept
{
    return data_->hasCalibrationTag && !data_->calibration && !data_->calibrationBakedIn;
}
bool IccTransform::calibrationBakedIn() const noexcept
{
    return data_->calibrationBakedIn;
}

bool IccTransform::validBuffers(std::span<const std::uint8_t> source,
                                 std::span<std::uint8_t> destination,
                                 std::uint16_t width, std::uint16_t height) noexcept
{
    if (!width || width > kMaximumActiveWidth || !height || height > kMaximumActiveHeight) return false;
    const auto bytes = std::size_t{width} * height * 3;
    if (source.size() < bytes || destination.size() < bytes) return false;
    const std::less<const std::uint8_t*> before;
    return !before(source.data(), destination.data() + bytes) ||
           !before(destination.data(), source.data() + bytes);
}

bool IccTransform::apply(std::span<const std::uint8_t> source,
                         std::span<std::uint8_t> destination,
                         std::uint16_t width, std::uint16_t height,
                         bool applyCalibration) const noexcept
{
    // The only supported layout is tightly packed BGR24.
    if (!validBuffers(source, destination, width, height)) return false;
    const auto pixels = std::size_t{width} * height;
    const auto bytes = pixels * 3;
    cmsDoTransform(data_->transform.get(), source.data(), destination.data(), pixels);
    if (applyCalibration && data_->calibration) {
        const auto& table = *data_->calibration;
        for (std::size_t i = 0; i < bytes; i += 3) {
            destination[i] = table[0][destination[i]];
            destination[i + 1] = table[1][destination[i + 1]];
            destination[i + 2] = table[2][destination[i + 2]];
        }
    }
    return true;
}

} // namespace mistercast
