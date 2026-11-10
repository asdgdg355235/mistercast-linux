#pragma once

#include "core/Modeline.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mistercast {

enum class CrtScan { Automatic, Progressive, Interlaced };

struct CrtInterval {
    double minimum{};
    double maximum{};
};

struct CrtLineRange {
    int minimum{};
    int maximum{};
};

struct CrtPorches {
    double front{};
    double sync{};
    double back{};
};

struct CrtProfile {
    CrtInterval horizontalHz;
    CrtInterval cadenceHz;
    CrtPorches horizontalUs;
    CrtPorches verticalMs;
    CrtLineRange progressiveLines;
    CrtLineRange interlacedLines;
    double preferredHorizontalHz{};
    double preferredCadenceHz{};
};

struct CrtRequest {
    // Zero uses source-informed dimensions or the selected profile's cadence.
    int width{};
    int height{};
    double cadenceHz{};
    CrtScan scan{CrtScan::Automatic};
    CrtProfile profile;
    CrtInterval clockMHz{2.5, 25.0};
    double refreshToleranceHz{0.25};
    std::string fallbackName;
};

struct CrtSourceSize {
    std::uint32_t width{};
    std::uint32_t height{};
    bool operator==(const CrtSourceSize&) const = default;
};

enum class CrtSelection { Manual, Generated, Fallback };

struct CrtModeResult {
    std::optional<Modeline> mode;
    CrtSelection selection{CrtSelection::Manual};
    std::vector<std::string> diagnostics;
    std::string error;
};

[[nodiscard]] std::optional<CrtProfile> crtProfile(std::string_view preset);
[[nodiscard]] std::string validateCrtMode(const Modeline& mode, const CrtRequest& request);
[[nodiscard]] CrtModeResult selectCrtMode(
    const std::optional<CrtRequest>& automatic, const Modeline& manual,
    std::span<const Modeline> catalog, CrtSourceSize source = {});

} // namespace mistercast
