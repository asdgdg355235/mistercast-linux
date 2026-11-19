#include "core/CrtGeometry.h"
#include "core/Gamma24Tables.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <cstring>
#include <functional>

namespace mistercast {
namespace {

struct GeometryWindow {
    int width, laneHeight, virtualWidth, virtualHeight, x, y, period;
};

GeometryWindow window(const Modeline& mode, CrtGeometrySettings settings)
{
    if (!settings.enabled) settings = {};
    const int width = mode.hActive;
    const int lanes = mode.interlaced ? 2 : 1;
    const int height = mode.vActive / lanes;
    const int period = 64 + settings.hSize;
    const int virtualHeight = height + 3 * settings.effectiveCabinetVSize();
    return {width, height, (width * period + 63) / 64, virtualHeight,
            settings.hPosition, std::max(0, height - virtualHeight) + settings.vShift, period};
}

// Split each power-of-two interval into 128 buckets. Their relative size
// is smaller than the minimum distance between consecutive gamma thresholds.
// The generated table verifies that each bucket crosses at most one threshold.
constexpr unsigned inverseShift(std::uint32_t light)
{
    return std::bit_width(light | 255u) - 8;
}

constexpr unsigned inverseIndex(std::uint32_t light, unsigned shift)
{
    return (shift << 7) + (light >> shift);
}

constexpr auto inverseThresholds = [] {
    std::array<std::uint32_t, 256> result{};
    std::copy(gamma24::thresholds.begin(), gamma24::thresholds.end(), result.begin());
    result.back() = std::numeric_limits<std::uint32_t>::max();
    return result;
}();

constexpr auto inverseBuckets = [] {
    std::array<std::uint8_t, 3328> buckets{};
    unsigned code = 0;
    for (std::uint64_t low = 0; low < (std::uint64_t{1} << 32);) {
        const auto shift = inverseShift(low);
        const auto next = low + (std::uint64_t{1} << shift);
        // A bucket records the code immediately below its first threshold.
        while (code < 255 && inverseThresholds[code] < low) ++code;
        if (code < 254 && inverseThresholds[code + 1] < next) {
            throw "inverse gamma bucket contains multiple thresholds";
        }
        buckets[inverseIndex(low, shift)] = static_cast<std::uint8_t>(code);
        low = next;
    }
    return buckets;
}();

std::uint8_t encode(std::uint32_t mixed)
{
    const auto code = inverseBuckets[inverseIndex(mixed, inverseShift(mixed))];
    return static_cast<std::uint8_t>(code + (mixed >= inverseThresholds[code]));
}

std::uint8_t blendFractional(std::uint8_t first, std::uint8_t second, unsigned weight)
{
    if (first == second) return first;
    // The weights sum to 256. The largest numerator is 256*(2^24-1).
    return encode((256u - weight) * gamma24::decode[first] + weight * gamma24::decode[second]);
}

} // namespace

int minimumCabinetSize(int laneHeight)
{
    return laneHeight > 0 ? std::max(CrtGeometrySettings::minCabinet, -(laneHeight - 1) / 3) : 0;
}

bool validCrtGeometryRaster(const Modeline& mode)
{
    return mode.hActive > 0 && mode.hActive <= kMaximumActiveWidth &&
        mode.vActive > 0 && mode.vActive <= kMaximumActiveHeight &&
        (!mode.interlaced || mode.vActive % 2 == 0);
}

CrtGeometrySettings clampCrtGeometry(CrtGeometrySettings s, const Modeline& mode)
{
    using S = CrtGeometrySettings;
    s.enabled = s.enabled && validCrtGeometryRaster(mode);
    s.hSize = std::clamp(s.hSize, S::minHSize, S::maxHSize);
    s.hPosition = std::clamp(s.hPosition, S::minHPosition, S::maxHPosition);
    s.vShift = std::clamp(s.vShift, S::minVShift, S::maxVShift);
    s.cabinetVSize = std::clamp(s.cabinetVSize,
        minimumCabinetSize(mode.vActive / (mode.interlaced ? 2 : 1)), S::maxCabinet);
    return s;
}

CrtGeometryBounds crtGeometryBounds(const Modeline& mode, CrtGeometrySettings settings)
{
    if (!validCrtGeometryRaster(mode)) return {};
    const auto g = window(mode, clampCrtGeometry(settings, mode));
    return {g.width, g.laneHeight, g.virtualWidth, g.virtualHeight,
        std::clamp(-g.x, 0, g.virtualWidth),
        std::clamp(g.x + g.virtualWidth - g.width, 0, g.virtualWidth),
        std::clamp(-g.y, 0, g.virtualHeight),
        std::clamp(g.y + g.virtualHeight - g.laneHeight, 0, g.virtualHeight)};
}

std::optional<CrtGeometryPlan> CrtGeometryPlan::make(const Modeline& mode, CrtGeometrySettings settings)
{
    if (!validCrtGeometryRaster(mode) || settings != clampCrtGeometry(settings, mode)) {
        return std::nullopt;
    }

    const auto g = window(mode, settings);
    CrtGeometryPlan plan;
    plan.width_ = mode.hActive;
    plan.height_ = mode.vActive;
    plan.begin_ = std::clamp(g.x, 0, g.width);
    plan.end_ = std::clamp(g.x + g.virtualWidth, 0, g.width);
    plan.contiguous_ = g.period == 64;
    for (unsigned x = plan.begin_; x < plan.end_; ++x) {
        plan.columnOffsets_[x] = 3 * (64 * (std::int64_t{x} - g.x) / g.period);
    }

    const int lanes = mode.interlaced ? 2 : 1;
    const int step = 256 * g.laneHeight / g.virtualHeight;
    for (int y = 0; y < mode.vActive; ++y) {
        const int n = y / lanes - g.y;
        if (n < 0 || n >= g.virtualHeight) continue;
        const int position = n * step;
        const int first = std::min(position / 256, g.laneHeight - 1);
        const int second = std::min(first + 1, g.laneHeight - 1);
        const int parity = y % lanes;
        plan.rows_[y] = {lanes * first + parity, lanes * second + parity,
                        first == second ? 0u : static_cast<unsigned>(position % 256)};
    }
    return plan;
}

int CrtGeometryPlan::sourceColumn(unsigned x) const
{
    return x >= begin_ && x < end_ ? columnOffsets_[x] / 3 : -1;
}

bool CrtGeometryPlan::validBuffers(std::span<const std::uint8_t> source,
                                   std::span<std::uint8_t> destination) const
{
    const auto bytes = std::size_t{width_} * height_ * 3;
    const std::less<const std::uint8_t*> before;
    return source.size() >= bytes && destination.size() >= bytes &&
        (!before(source.data(), destination.data() + bytes) ||
         !before(destination.data(), source.data() + bytes));
}

void CrtGeometryPlan::render(std::span<const std::uint8_t> source,
                            std::span<std::uint8_t> destination, unsigned first, unsigned end) const
{
    const auto pitch = std::size_t{width_} * 3;
    for (unsigned y = first; y < end; ++y) {
        const auto row = rows_[y];
        auto* out = destination.data() + y * pitch;
        if (row.first < 0 || begin_ == end_) {
            std::fill_n(out, pitch, 0);
            continue;
        }
        std::fill_n(out, begin_ * 3, 0);
        std::fill_n(out + end_ * 3, (width_ - end_) * 3, 0);
        const auto* a = source.data() + row.first * pitch;
        if (!row.weight && contiguous_) {
            std::memcpy(out + begin_ * 3, a + columnOffsets_[begin_], (end_ - begin_) * 3);
        } else if (!row.weight) {
            for (auto x = begin_; x < end_; ++x) {
                std::memcpy(out + x * 3, a + columnOffsets_[x], 3);
            }
        } else {
            const auto* b = source.data() + row.second * pitch;
            for (auto x = begin_; x < end_; ++x) {
                const auto offset = columnOffsets_[x];
                for (int channel = 0; channel < 3; ++channel) {
                    out[x * 3 + channel] = blendFractional(a[offset + channel], b[offset + channel], row.weight);
                }
            }
        }
    }
}

std::uint8_t blendGamma24(std::uint8_t first, std::uint8_t second, unsigned weight)
{
    if (!weight) return first;
    if (weight == 256) return second;
    return blendFractional(first, second, weight);
}

} // namespace mistercast
