#pragma once

#include "core/Scaler.h"

#include <memory>

namespace mistercast {

// Synchronous, one-frame executor. Source and destination remain caller-owned
// until scale returns. Concurrent scale calls are rejected, never queued.
class ScalingExecutor {
public:
    static std::size_t defaultWorkerCount();
    explicit ScalingExecutor(std::size_t workers = defaultWorkerCount());
    ~ScalingExecutor();
    ScalingExecutor(const ScalingExecutor&) = delete;
    ScalingExecutor& operator=(const ScalingExecutor&) = delete;

    bool scale(const SourceFrame& source, const ScalingGeometry& geometry,
               ScalingAlgorithm algorithm, std::span<std::uint8_t> destination);
    std::size_t workerCount() const;
    // Cumulative thread starts for this executor's lifetime, also usable by
    // diagnostics/tests to distinguish persistent workers from per-frame ones.
    std::size_t threadsStarted() const;
    bool isStopped() const;
    // Reject new frames, finish the active operation, wake and join all workers.
    // Idempotent; may overlap scale(), but destruction requires caller quiescence.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
