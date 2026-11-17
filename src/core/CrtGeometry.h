#pragma once

#include "core/Modeline.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace mistercast {

struct CrtGeometrySettings {
    static constexpr int algorithmVersion = 1;
    static constexpr int minHSize = -16, maxHSize = 15;
    static constexpr int minHPosition = -48, maxHPosition = 48;
    static constexpr int minVShift = -16, maxVShift = 15;
    static constexpr int minCabinet = -7, maxCabinet = 7;

    bool enabled{};
    int hSize{}, hPosition{}, vShift{}, cabinetVSize{};

    bool effective() const { return enabled && (hSize || hPosition || vShift || cabinetVSize); }
    bool operator==(const CrtGeometrySettings&) const = default;
};

struct CrtGeometryBounds {
    int width{}, laneHeight{}, virtualWidth{}, virtualHeight{};
    int left{}, right{}, top{}, bottom{}; // Clipped pixels or field-lines.
};

int minimumCabinetSize(int laneHeight);
bool validCrtGeometryRaster(const Modeline& mode);
CrtGeometrySettings clampCrtGeometry(CrtGeometrySettings settings, const Modeline& mode);
CrtGeometryBounds crtGeometryBounds(const Modeline& mode, CrtGeometrySettings settings);

class CrtGeometryPlan {
public:
    struct Row {
        int first{-1}, second{-1};
        unsigned weight{}; // Q8 weight on second.
    };

    static std::optional<CrtGeometryPlan> make(const Modeline& mode, CrtGeometrySettings settings);
    unsigned width() const { return width_; }
    unsigned height() const { return height_; }
    int sourceColumn(unsigned x) const;
    Row sourceRow(unsigned y) const { return rows_[y]; }
    bool validBuffers(std::span<const std::uint8_t> source,
                      std::span<std::uint8_t> destination) const;

    // Call with validated, nonoverlapping buffers. Workers write disjoint rows.
    void render(std::span<const std::uint8_t> source, std::span<std::uint8_t> destination,
                unsigned first, unsigned end) const;

private:
    CrtGeometryPlan() = default;
    std::array<int, kMaximumActiveWidth> columnOffsets_{};
    std::array<Row, kMaximumActiveHeight> rows_{};
    unsigned width_{}, height_{}, begin_{}, end_{};
    bool contiguous_{};
};

// weight is an integer in [0,256].
std::uint8_t blendGamma24(std::uint8_t first, std::uint8_t second, unsigned weight);

} // namespace mistercast
