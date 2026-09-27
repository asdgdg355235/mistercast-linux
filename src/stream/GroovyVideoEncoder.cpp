#include "stream/GroovyVideoEncoder.h"

#include <lz4.h>

#include <algorithm>

namespace mistercast {

GroovyVideoEncoder::GroovyVideoEncoder()
    : fullLz4_(LZ4_compressBound(kMaximumFrameBytes))
    , delta_(kMaximumFrameBytes)
    , deltaLz4_(fullLz4_.size())
{
    for (auto& history : history_) {
        history.raw.resize(kMaximumFrameBytes);
    }
}

void GroovyVideoEncoder::invalidate()
{
    for (auto& history : history_) {
        history.valid = false;
        history.identity = {};
        history.deltasSinceFull = 0;
    }
    identity_ = {};
}

GroovyVideoEncoder::EncodedFrame GroovyVideoEncoder::prepare(
    std::span<const std::uint8_t> raw, const Modeline& mode, FieldParity field)
{
    const auto slot = static_cast<unsigned>(field);
    if (slot > 1 || raw.empty() || raw.size() > kMaximumFrameBytes ||
        raw.size() != mode.payloadBytes() || (!mode.interlaced && slot != 0)) {
        invalidate();
        return {};
    }
    const Identity identity{raw.size(), mode.hActive, mode.payloadHeight(), mode.interlaced};
    if (identity != identity_) {
        invalidate();
        identity_ = identity;
    }

    const int fullSize = LZ4_compress_default(
        reinterpret_cast<const char*>(raw.data()), fullLz4_.data(),
        static_cast<int>(raw.size()), static_cast<int>(fullLz4_.size()));
    if (fullSize <= 0) {
        return {};
    }
    EncodedFrame result{
        std::span(reinterpret_cast<const std::uint8_t*>(fullLz4_.data()), fullSize)};
    const auto& history = history_[slot];
    if (!history.valid || history.identity != identity) {
        return result;
    }
    if (history.deltasSinceFull >= kFullRefreshInterval - 1) {
        result.forcedFull = true;
        return result;
    }

    std::uint64_t unchanged = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        delta_[i] = static_cast<std::uint8_t>(raw[i] - history.raw[i]);
        unchanged += delta_[i] == 0;
    }
    // Original MiSTerCast CmdBlit content gate, expressed as exact integer
    // inequalities: full/raw > .05, match/raw > .20, (match+full)/raw > .90.
    const auto full = static_cast<std::uint64_t>(fullSize);
    const auto bytes = static_cast<std::uint64_t>(raw.size());
    if (full * 100 <= bytes * 5 || unchanged * 100 <= bytes * 20 ||
        (unchanged + full) * 100 <= bytes * 90) {
        result.rejection = Rejection::Content;
        return result;
    }
    const int deltaSize = LZ4_compress_default(
        reinterpret_cast<const char*>(delta_.data()), deltaLz4_.data(),
        static_cast<int>(raw.size()), static_cast<int>(deltaLz4_.size()));
    if (deltaSize > 0 && static_cast<std::uint64_t>(deltaSize) * 100 < full * 95) {
        result.payload = std::span(
            reinterpret_cast<const std::uint8_t*>(deltaLz4_.data()), deltaSize);
        result.delta = true;
    } else {
        result.rejection = Rejection::Size;
    }
    return result;
}

void GroovyVideoEncoder::commit(std::span<const std::uint8_t> raw,
    FieldParity field, const EncodedFrame& encoded)
{
    const auto slot = static_cast<unsigned>(field);
    if (slot > 1 || encoded.payload.empty() || raw.size() != identity_.bytes ||
        raw.size() > kMaximumFrameBytes) {
        invalidate();
        return;
    }
    auto& history = history_[slot];
    std::copy(raw.begin(), raw.end(), history.raw.begin());
    history.identity = identity_;
    history.valid = true;
    history.deltasSinceFull = encoded.delta ? history.deltasSinceFull + 1 : 0;
}

} // namespace mistercast
