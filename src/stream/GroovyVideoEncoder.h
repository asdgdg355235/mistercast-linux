#pragma once

#include "core/Modeline.h"
#include "core/StreamTypes.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace mistercast {

// Single-owner encoder. prepare() never promotes a candidate to history;
// the transport must call commit() only after submitting the entire payload.
class GroovyVideoEncoder {
public:
    // GroovyRelay: 29 deltas, then a full field on opportunity 30.
    static constexpr unsigned kFullRefreshInterval = 30;

    enum class Rejection { None, Content, Size };
    struct EncodedFrame {
        std::span<const std::uint8_t> payload;
        bool delta{};
        bool forcedFull{};
        Rejection rejection{};
    };

    GroovyVideoEncoder();
    [[nodiscard]] EncodedFrame prepare(
        std::span<const std::uint8_t> raw, const Modeline& mode, FieldParity field);
    void commit(std::span<const std::uint8_t> raw, FieldParity field,
        const EncodedFrame& encoded);
    void invalidate();

private:
    struct Identity {
        std::size_t bytes{};
        std::uint16_t width{};
        std::uint16_t height{};
        bool interlaced{};
        bool operator==(const Identity&) const = default;
    };
    struct History {
        std::vector<std::uint8_t> raw;
        Identity identity{};
        unsigned deltasSinceFull{};
        bool valid{};
    };

    std::vector<char> fullLz4_;
    std::vector<std::uint8_t> delta_;
    std::vector<char> deltaLz4_;
    std::array<History, 2> history_;
    Identity identity_{};
};

} // namespace mistercast
