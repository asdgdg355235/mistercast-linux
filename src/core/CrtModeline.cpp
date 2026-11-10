#include "core/CrtModeline.h"

#include "core/ModelineCatalog.h"
#include "modeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace mistercast {
namespace {

constexpr double kFrequencyEpsilon = 1e-7;
constexpr int kMaximumBlankingPart = 255; // Groovy HPS-to-FPGA timing registers.
constexpr int kMaximumVerticalTotal = kMaximumActiveHeight + 3 * kMaximumBlankingPart;

bool within(double value, CrtInterval interval)
{
    return std::isfinite(value) && value >= interval.minimum - kFrequencyEpsilon &&
        value <= interval.maximum + kFrequencyEpsilon;
}

bool ordered(CrtInterval interval, double minimum, double maximum)
{
    return interval.minimum >= minimum && interval.maximum <= maximum &&
        interval.minimum <= interval.maximum;
}

bool validLines(CrtLineRange lines)
{
    return (lines.minimum == 0 && lines.maximum == 0) ||
        (lines.minimum >= 1 && lines.minimum <= lines.maximum && lines.maximum <= kMaximumActiveHeight);
}

bool validPorches(CrtPorches porches, double period)
{
    return porches.front > 0.0 && porches.sync > 0.0 && porches.back > 0.0 &&
        porches.front + porches.sync + porches.back < period;
}

bool containsLines(CrtLineRange lines, int height)
{
    return height > 0 && height >= lines.minimum && height <= lines.maximum;
}

std::string validateRequest(const CrtRequest& request)
{
    const auto& p = request.profile;
    if (!ordered(p.horizontalHz, 14000.0, 540672.0) ||
        !ordered(p.cadenceHz, 40.0, 120.0) ||
        !within(p.preferredHorizontalHz, p.horizontalHz) ||
        !within(p.preferredCadenceHz, p.cadenceHz) ||
        !validLines(p.progressiveLines) || !validLines(p.interlacedLines) ||
        (p.progressiveLines.maximum == 0 && p.interlacedLines.maximum == 0) ||
        !validPorches(p.horizontalUs, 0.75e6 / p.horizontalHz.maximum) ||
        !validPorches(p.verticalMs, 0.75e3 / p.cadenceHz.maximum)) {
        return "Invalid CRT operating range, active lines or porch specification";
    }
    if (!ordered(request.clockMHz, 2.5, 25.0)) {
        return "Automatic pixel-clock limits must be ordered within 2.5–25 MHz";
    }
    if (!within(request.refreshToleranceHz, {0.0, 2.0})) {
        return "Refresh tolerance must be between 0 and 2 Hz";
    }
    if (request.width < 0 || request.width > kMaximumActiveWidth ||
        request.height < 0 || request.height > kMaximumActiveHeight) {
        return "Requested output exceeds the 720x576 limit";
    }
    if (request.cadenceHz != 0.0 &&
        !within(request.cadenceHz, {p.cadenceHz.minimum - request.refreshToleranceHz,
                                   p.cadenceHz.maximum + request.refreshToleranceHz})) {
        return "Requested refresh is outside the CRT profile and refresh tolerance";
    }
    return {};
}

monitor_range switchresRange(const CrtProfile& p)
{
    monitor_range range{};
    range.hfreq_min = p.horizontalHz.minimum;
    range.hfreq_max = p.horizontalHz.maximum;
    range.vfreq_min = p.cadenceHz.minimum;
    range.vfreq_max = p.cadenceHz.maximum;
    range.hfront_porch = p.horizontalUs.front;
    range.hsync_pulse = p.horizontalUs.sync;
    range.hback_porch = p.horizontalUs.back;
    range.vfront_porch = p.verticalMs.front / 1000.0;
    range.vsync_pulse = p.verticalMs.sync / 1000.0;
    range.vback_porch = p.verticalMs.back / 1000.0;
    range.vertical_blank = range.vfront_porch + range.vsync_pulse + range.vback_porch;
    range.progressive_lines_min = p.progressiveLines.minimum;
    range.progressive_lines_max = p.progressiveLines.maximum;
    range.interlaced_lines_min = p.interlacedLines.minimum;
    range.interlaced_lines_max = p.interlacedLines.maximum;
    return range;
}

struct OutputRequest {
    int width{};
    int height{};
    double cadence{};
    bool interlaced{};
};

OutputRequest outputRequest(const CrtRequest& request, CrtSourceSize source)
{
    const auto& p = request.profile;
    const auto cadence = request.cadenceHz == 0.0 ? p.preferredCadenceHz : request.cadenceHz;
    const bool isPal = cadence < 55.0;
    const bool useSourceSize = source.width > 0 && source.width <= kMaximumActiveWidth &&
        source.height > 0 && source.height <= kMaximumActiveHeight;
    const auto sourceHeight = useSourceSize ? static_cast<int>(source.height) : 0;
    const auto height = request.height != 0 ? request.height : sourceHeight;

    const bool interlaced = request.scan == CrtScan::Interlaced ||
        (request.scan == CrtScan::Automatic &&
         !containsLines(p.progressiveLines, height) &&
         (containsLines(p.interlacedLines, height) || p.progressiveLines.maximum == 0 ||
          (request.height == 0 && source.height > static_cast<unsigned>(p.progressiveLines.maximum) &&
           p.interlacedLines.maximum > 0)));
    const auto lines = interlaced ? p.interlacedLines : p.progressiveLines;
    const int standardHeight = interlaced ? (isPal ? 576 : 480) : (isPal ? 288 : 240);
    int inferredHeight = containsLines(lines, sourceHeight) && (!interlaced || sourceHeight % 2 == 0)
        ? sourceHeight : std::clamp(standardHeight, lines.minimum, lines.maximum);
    if (interlaced && inferredHeight % 2 != 0) {
        inferredHeight += inferredHeight < lines.maximum ? 1 : -1;
    }

    const int standardWidth = interlaced ? (isPal ? 720 : 640) : 320;
    return {
        request.width != 0 ? request.width :
            (useSourceSize ? static_cast<int>(source.width) : standardWidth),
        request.height != 0 ? request.height : inferredHeight,
        cadence, interlaced,
    };
}

std::array<int, 3> blankingParts(int active, int begin, int end, int total)
{
    return {begin - active, end - begin, total - end};
}

bool validBlanking(const std::array<int, 3>& parts, CrtPorches nominal,
                   double timePerUnit, double fraction, double rounding)
{
    const std::array<double, 3> minimum{nominal.front, nominal.sync, nominal.back};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] < 1 || parts[i] > kMaximumBlankingPart ||
            (parts[i] + rounding) * timePerUnit + kFrequencyEpsilon < minimum[i] * fraction) {
            return false;
        }
    }
    return true;
}

double realizableClockMHz(double pixelClockMHz)
{
    // Groovy scales the pixel clock to the PLL input, then searches dividers
    // for a VCO above 400 MHz. Fractional multipliers near integers are
    // forbidden. If the search fails, the firmware uses an integer multiplier.
    const int pixelDivider = pixelClockMHz * 16 < 90 ? 16 :
        pixelClockMHz * 12 < 90 ? 12 : pixelClockMHz * 8 < 100 ? 8 :
        pixelClockMHz * 6 < 100 ? 6 : 4;
    const double clock = pixelClockMHz * pixelDivider;
    const int first = static_cast<int>(std::ceil(400.0 / clock));
    for (int divider = first;; ++divider) {
        const double vco = clock * divider;
        const double multiplier = vco / 50.0;
        const double fraction = multiplier - std::floor(multiplier);
        if (fraction == 0.0 || (fraction > 0.05f && fraction < 0.95f)) {
            return pixelClockMHz;
        }
        if (vco > 1500.0) {
            break;
        }
    }
    return std::round(clock * first / 50.0) * 50.0 / (first * pixelDivider);
}

std::string validateTiming(const Modeline& mode, const CrtRequest& request, double target)
{
    std::string reason;
    if (!ModelineCatalog::validate(mode, reason)) {
        return reason;
    }
    if (mode.interlaced && mode.vTotal % 2 == 0) {
        return "Interlaced timing requires an odd full-frame line total";
    }
    const auto& p = request.profile;
    const double horizontalHz = mode.horizontalHz();
    if (!within(horizontalHz, p.horizontalHz) || !within(mode.cadenceHz(), p.cadenceHz)) {
        return "Horizontal frequency or field/frame cadence is outside the CRT profile";
    }
    if (!within(mode.pixelClockMHz, request.clockMHz)) {
        return "Pixel clock is outside configured limits";
    }
    if (!within(mode.cadenceHz(), {target - request.refreshToleranceHz,
                                   target + request.refreshToleranceHz})) {
        return "Achieved cadence is outside the requested refresh tolerance";
    }
    if (!containsLines(mode.interlaced ? p.interlacedLines : p.progressiveLines, mode.vActive)) {
        return "Active lines are outside the CRT profile for this scan type";
    }
    if (!validBlanking(blankingParts(mode.hActive, mode.hBegin, mode.hEnd, mode.hTotal),
            p.horizontalUs, 1.0 / mode.pixelClockMHz, 0.9, 0.0)) {
        return "Horizontal porch or sync is too short or exceeds an 8-bit FPGA register";
    }
    if (!validBlanking(blankingParts(mode.vActive, mode.vBegin, mode.vEnd, mode.vTotal),
            p.verticalMs, 1000.0 / horizontalHz / (mode.interlaced ? 2.0 : 1.0), 1.0, 1.0)) {
        return "Vertical porch or sync is too short or exceeds an 8-bit FPGA register";
    }
    return {};
}

std::string validatePresetTiming(const Modeline& mode, const CrtRequest& request, double target)
{
    auto reason = validateTiming(mode, request, target);
    if (!reason.empty()) {
        return reason;
    }
    const auto effectiveClock = realizableClockMHz(mode.pixelClockMHz);
    if (effectiveClock != mode.pixelClockMHz) {
        auto effective = mode;
        effective.pixelClockMHz = effectiveClock;
        reason = validateTiming(effective, request, target);
        if (!reason.empty()) {
            return "Groovy PLL rounds the preset clock outside the limits: " + reason;
        }
    }
    return {};
}

std::optional<Modeline> generate(const CrtRequest& request, const OutputRequest& output,
                                std::vector<std::string>& diagnostics)
{
    const auto& p = request.profile;
    if (!containsLines(output.interlaced ? p.interlacedLines : p.progressiveLines, output.height) ||
        (output.interlaced && output.height % 2 != 0)) {
        diagnostics.push_back("Generation rejected: active height or scan type is outside the CRT profile");
        return std::nullopt;
    }

    generator_settings settings{};
    settings.interlace = output.interlaced;
    settings.monitor_aspect = 4.0 / 3.0;
    settings.refresh_tolerance = request.refreshToleranceHz;
    settings.super_width = std::numeric_limits<int>::max();
    settings.pixel_precision = 1;

    const double fields = output.interlaced ? 2.0 : 1.0;
    const int step = output.interlaced ? 2 : 1;
    const double minRate = std::max(p.cadenceHz.minimum, output.cadence - request.refreshToleranceHz);
    const double maxRate = std::min(p.cadenceHz.maximum, output.cadence + request.refreshToleranceHz);
    if (minRate > maxRate) {
        return std::nullopt;
    }
    const auto scoreFor = [&](double rate, double horizontalHz) {
        const auto error = std::abs(rate - output.cadence);
        return std::tuple{error < kFrequencyEpsilon ? 0.0 : error,
                          std::abs(horizontalHz - p.preferredHorizontalHz)};
    };
    auto bestScore = std::tuple{std::numeric_limits<double>::infinity(), 0.0};
    std::optional<Modeline> best;

    // Pin Switchres's minimum line count for each feasible raster. Score the
    // result against the original monitor limits, including the half line.
    const int firstTotal = std::max(output.height + 1,
        static_cast<int>(std::ceil(p.horizontalHz.minimum * fields / maxRate - kFrequencyEpsilon)));
    const int lastTotal = std::min(kMaximumVerticalTotal,
        static_cast<int>(std::floor(p.horizontalHz.maximum * fields / minRate + kFrequencyEpsilon)));
    for (int total = firstTotal + (output.interlaced && firstTotal % 2 == 0); total <= lastTotal; total += step) {
        const double linesPerField = total / fields;
        const double minimum = std::max(minRate, p.horizontalHz.minimum / linesPerField);
        const double maximum = std::min(maxRate, p.horizontalHz.maximum / linesPerField);
        if (minimum > maximum) {
            continue;
        }
        const double rate = std::clamp(output.cadence, minimum, maximum);
        const double horizontalHz = rate * linesPerField;
        if (scoreFor(rate, horizontalHz) >= bestScore) {
            continue;
        }

        auto range = switchresRange(p);
        range.hfreq_min = (total / step - 0.5) * rate;
        // The upstream integer-line loop uses a strict comparison at its upper bound.
        range.hfreq_max = horizontalHz + kFrequencyEpsilon;
        modeline source{};
        source.hactive = output.width;
        source.vactive = output.height;
        source.vfreq = rate;
        source.interlace = output.interlaced;
        auto calculated = source;
        calculated.type = V_FREQ_EDITABLE;

        const auto rejected = [&](const std::string& why) {
            diagnostics.push_back("Rejected Switchres candidate with " + std::to_string(total) +
                                  " total lines: " + why);
        };
        if (modeline_create(&source, &calculated, &range, &settings) != 0) {
            rejected("Switchres could not generate timings");
            continue;
        }
        if (calculated.doublescan || calculated.hsync || calculated.vsync ||
            calculated.hactive != output.width || calculated.vactive != output.height ||
            calculated.interlace != output.interlaced || calculated.vtotal != total) {
            rejected("Switchres changed the requested raster or returned unsupported flags");
            continue;
        }
        const std::array positions{calculated.hbegin, calculated.hend, calculated.htotal,
                                   calculated.vbegin, calculated.vend, calculated.vtotal};
        if (std::ranges::any_of(positions, [](int value) { return value < 1 || value > UINT16_MAX; })) {
            rejected("A timing position exceeds the mode packet");
            continue;
        }
        const double pixelClockMHz = calculated.htotal * horizontalHz / 1e6;
        if (!within(pixelClockMHz, {2.5, 25.0})) {
            rejected("Pixel clock exceeds the Groovy automatic clock envelope");
            continue;
        }
        Modeline mode{
            std::to_string(output.width) + "x" + std::to_string(output.height) +
                (output.interlaced ? "i" : "p") + " (Switchres)",
            realizableClockMHz(pixelClockMHz),
            static_cast<std::uint16_t>(calculated.hactive),
            static_cast<std::uint16_t>(calculated.hbegin),
            static_cast<std::uint16_t>(calculated.hend),
            static_cast<std::uint16_t>(calculated.htotal),
            static_cast<std::uint16_t>(calculated.vactive),
            static_cast<std::uint16_t>(calculated.vbegin),
            static_cast<std::uint16_t>(calculated.vend),
            static_cast<std::uint16_t>(calculated.vtotal),
            output.interlaced,
        };
        if (auto reason = validateTiming(mode, request, output.cadence); !reason.empty()) {
            rejected(reason);
            continue;
        }
        if (const auto score = scoreFor(mode.cadenceHz(), mode.horizontalHz()); score < bestScore) {
            bestScore = score;
            best = std::move(mode);
        }
    }
    return best;
}

} // namespace

std::optional<CrtProfile> crtProfile(std::string_view preset)
{
    if (preset != "ntsc" && preset != "pal" && preset != "generic_15" && preset != "arcade_15") {
        return std::nullopt;
    }

    std::array<monitor_range, MAX_RANGES> ranges{};
    const std::string name(preset);
    if (monitor_set_preset(name.c_str(), ranges.data()) != 1 || ranges.front().hfreq_min == 0) {
        return std::nullopt;
    }
    const auto& r = ranges.front();
    CrtProfile result{
        {r.hfreq_min, r.hfreq_max}, {r.vfreq_min, r.vfreq_max},
        {r.hfront_porch, r.hsync_pulse, r.hback_porch},
        {r.vfront_porch * 1000.0, r.vsync_pulse * 1000.0, r.vback_porch * 1000.0},
        {r.progressive_lines_min, r.progressive_lines_max},
        {r.interlaced_lines_min, r.interlaced_lines_max},
        r.hfreq_min, 59.94,
    };
    // The upstream TV presets are single frequency points. Allow the narrow
    // range needed by integer-line 240p/288p and the bundled TV modelines.
    if (preset == "ntsc") {
        result.horizontalHz = {15700.0, 15760.0};
        result.cadenceHz = {59.7, 60.3};
    } else if (preset == "pal") {
        result.horizontalHz = {15575.0, 15675.0};
        result.cadenceHz = {49.7, 50.3};
        result.preferredCadenceHz = 50.0;
    } else {
        result.preferredHorizontalHz = 15734.26;
    }
    return result;
}

std::string validateCrtMode(const Modeline& mode, const CrtRequest& request)
{
    if (auto reason = validateRequest(request); !reason.empty()) {
        return reason;
    }
    if ((request.width != 0 && mode.hActive != request.width) ||
        (request.height != 0 && mode.vActive != request.height) ||
        (request.scan == CrtScan::Progressive && mode.interlaced) ||
        (request.scan == CrtScan::Interlaced && !mode.interlaced)) {
        return "Scan type or active area differs from the request";
    }
    return validatePresetTiming(mode, request, request.cadenceHz == 0.0 ?
        request.profile.preferredCadenceHz : request.cadenceHz);
}

CrtModeResult selectCrtMode(const std::optional<CrtRequest>& automatic, const Modeline& manual,
                            std::span<const Modeline> catalog, CrtSourceSize source)
{
    if (!automatic) {
        return {manual, CrtSelection::Manual, {}, {}};
    }
    const auto& request = *automatic;
    CrtModeResult result;
    if (auto reason = validateRequest(request); !reason.empty()) {
        result.diagnostics.push_back("Generation rejected: " + reason);
        result.error = std::move(reason);
        return result;
    }

    const auto output = outputRequest(request, source);
    const bool alternateScan = request.scan == CrtScan::Automatic && output.height % 2 == 0 &&
        containsLines(output.interlaced ? request.profile.progressiveLines :
            request.profile.interlacedLines, output.height);
    auto generated = generate(request, output, result.diagnostics);
    if (!generated && alternateScan) {
        auto alternate = output;
        alternate.interlaced = !output.interlaced;
        generated = generate(request, alternate, result.diagnostics);
    }
    if (generated) {
        result.mode = std::move(generated);
        result.selection = CrtSelection::Generated;
        return result;
    }
    result.diagnostics.push_back("Switchres found no valid timing for " +
        std::to_string(output.width) + "x" + std::to_string(output.height) +
        (output.interlaced ? "i at " : "p at ") + std::to_string(output.cadence) + " Hz");

    const auto check = [&](const Modeline& mode) {
        std::string reason;
        if ((!alternateScan && mode.interlaced != output.interlaced) || mode.vActive != output.height ||
            (request.width != 0 && mode.hActive != output.width)) {
            reason = "scan type or active area differs from the request";
        } else {
            reason = validatePresetTiming(mode, request, output.cadence);
        }
        if (!reason.empty()) {
            result.diagnostics.push_back("Rejected fallback " + mode.name + ": " + reason);
        }
        return reason.empty();
    };
    const auto use = [&](const Modeline& mode, std::string_view kind) {
        result.mode = mode;
        result.selection = CrtSelection::Fallback;
        result.diagnostics.push_back("Using " + std::string(kind) + " fallback " + mode.name);
    };

    auto preferred = catalog.end();
    if (!request.fallbackName.empty()) {
        preferred = std::ranges::find(catalog, request.fallbackName, &Modeline::name);
        if (preferred == catalog.end()) {
            result.diagnostics.push_back("Selected fallback is unavailable: " + request.fallbackName);
        } else if (check(*preferred)) {
            use(*preferred, "selected");
            return result;
        }
    }

    const Modeline* best = nullptr;
    const auto score = [&](const Modeline& mode) {
        return std::tuple{mode.interlaced != output.interlaced,
                          std::abs(mode.hActive - output.width), std::abs(mode.cadenceHz() - output.cadence)};
    };
    for (auto it = catalog.begin(); it != catalog.end(); ++it) {
        if (it != preferred && check(*it) && (!best || score(*it) < score(*best))) {
            best = &*it;
        }
    }
    if (best) {
        use(*best, "catalog");
    } else if (check(kDefaultModeline)) {
        use(kDefaultModeline, "built-in");
    } else {
        result.error = "No safe CRT modeline matches the request. Check the timing preview or select a manual preset.";
    }
    return result;
}

} // namespace mistercast
