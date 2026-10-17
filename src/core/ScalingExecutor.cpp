#include "core/ScalingExecutor.h"

#include "core/FrameProcessor.h"
#include "core/LancIRRuntime.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace mistercast {

struct ScalingExecutor::Impl {
    struct Worker {
        std::optional<detail::LancIRRuntime> lancir{std::in_place};
        std::thread thread;
    };
    enum class Phase { Native, CopyCrop, LancIR, Transform };
    struct Job {
        Phase phase{};
        const ScalePlan* plan{};
        ScalingGeometry geometry{};
        ScalingInput input{};
        std::span<std::uint8_t> destination;
        std::uint8_t* scaled{};
        int preWidth{}, preHeight{};
        std::uint32_t rows{};
    } job;

    const std::size_t count;
    std::atomic_size_t starts{};
    std::atomic_bool accepting{true}, stopped{};
    std::mutex operationMutex; // Covers all phases and scratch ownership.
    std::mutex dispatchMutex;
    std::condition_variable wake, complete;
    std::uint64_t generation{};
    std::size_t active{}, remaining{};
    bool stopping{}, failed{};
    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<std::uint8_t> packedCrop;
    std::array<std::uint8_t, kMaximumActiveWidth * kMaximumActiveHeight * 4> scaled;

    explicit Impl(std::size_t requested) : count(std::max(std::size_t{1}, requested))
    {
        workers.reserve(count);
        // Finish creating contexts before starting threads. On a thread-start
        // failure, join those already running before their contexts are freed.
        for (std::size_t i = 0; i < count; ++i) workers.push_back(std::make_unique<Worker>());
        try {
            for (std::size_t i = 0; i < count; ++i) {
                workers[i]->thread = std::thread([this, i] { run(i); });
                ++starts;
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~Impl() { shutdown(); }

    void shutdown()
    {
        accepting.store(false);
        std::lock_guard operation(operationMutex);
        if (stopped.load()) return;
        {
            std::lock_guard lock(dispatchMutex);
            stopping = true;
        }
        wake.notify_all();
        for (auto& worker : workers) {
            if (worker->thread.joinable()) worker->thread.join();
            worker->lancir.reset();
        }
        stopped.store(true);
    }

    void run(std::size_t index)
    {
        std::uint64_t seen = 0;
        std::unique_lock lock(dispatchMutex);
        for (;;) {
            wake.wait(lock, [&] { return stopping || generation != seen; });
            if (stopping) return;
            seen = generation;
            if (index >= active) continue;
            const auto first = static_cast<std::uint32_t>(job.rows * index / active);
            const auto end = static_cast<std::uint32_t>(job.rows * (index + 1) / active);
            lock.unlock();
            bool success = false;
            try {
                success = execute(*workers[index], first, end);
            } catch (...) {
                // A failed upstream allocation may invalidate its cache. Only
                // exceptional failure discards a context; the next frame can
                // recreate it. No partial frame is published by either caller.
                workers[index]->lancir.reset();
            }
            lock.lock();
            failed = failed || !success;
            if (--remaining == 0) complete.notify_one();
        }
    }

    bool dispatch(Phase phase, std::uint32_t rows)
    {
        std::unique_lock lock(dispatchMutex);
        job.phase = phase;
        job.rows = rows;
        active = std::min(count, std::size_t{rows});
        remaining = active;
        failed = false;
        ++generation;
        wake.notify_all();
        complete.wait(lock, [&] { return remaining == 0; });
        return !failed;
    }

    bool execute(Worker& worker, std::uint32_t first, std::uint32_t end)
    {
        const auto& g = job.geometry;
        const auto& in = job.input;
        switch (job.phase) {
        case Phase::Native:
            return scaleRows(*job.plan, job.destination, first, end);
        case Phase::CopyCrop: {
            const auto rowBytes = std::size_t{g.cropWidth} * in.channels;
            for (auto y = first; y < end; ++y) {
                std::copy_n(in.pixels + std::int64_t{y} * in.stride, rowBytes,
                            packedCrop.data() + y * rowBytes);
            }
            return true;
        }
        case Phase::LancIR:
            if (!worker.lancir) worker.lancir.emplace();
            return worker.lancir->resizeStrip(in.pixels, g.cropWidth, g.cropHeight,
                static_cast<int>(in.stride),
                job.scaled + std::size_t{first} * job.preWidth * in.channels,
                job.preWidth, job.preHeight, in.channels, first, end) == int(end - first);
        case Phase::Transform:
            for (auto y = first; y < end; ++y) {
                for (std::uint32_t x = 0; x < g.outputWidth; ++x) {
                    std::uint32_t sx = x, sy = y;
                    switch (g.rotation) {
                    case Rotation::None: break;
                    case Rotation::Flip180:
                        sx = g.outputWidth - 1 - x; sy = g.outputHeight - 1 - y; break;
                    case Rotation::Clockwise90:
                        sx = y; sy = g.outputWidth - 1 - x; break;
                    case Rotation::CounterClockwise90:
                        sx = g.outputHeight - 1 - y; sy = x; break;
                    }
                    const auto* p = job.scaled + (std::size_t{sy} * job.preWidth + sx) * in.channels;
                    auto* out = job.destination.data() + (std::size_t{y} * g.outputWidth + x) * 3;
                    out[0] = p[in.blue]; out[1] = p[1]; out[2] = p[in.red];
                }
            }
            return true;
        }
        return false;
    }

    bool scale(const SourceFrame& source, const ScalingGeometry& geometry,
               ScalingAlgorithm algorithm, std::span<std::uint8_t> destination)
    {
        if (!accepting.load()) return false;
        std::unique_lock operation(operationMutex, std::try_to_lock);
        if (!operation || !accepting.load()) return false;
        const auto& descriptor = scalerDescriptor(algorithm);
        if (descriptor.algorithm != algorithm || destination.size() <
                std::size_t{geometry.outputWidth} * geometry.outputHeight * 3) return false;
        try {
            job.geometry = geometry;
            job.destination = destination;
            switch (descriptor.backend) {
            case ScalerBackend::NativeRows: {
                ScalePlan plan;
                if (!prepareScalePlan(source, geometry, algorithm, plan)) return false;
                job.plan = &plan;
                return dispatch(Phase::Native, geometry.outputHeight);
            }
            case ScalerBackend::LancIR:
                if (!prepareScalingInput(source, geometry, job.input)) return false;
                // LANCIR uses signed int dimensions, strides and kernel sizes.
                // Leave headroom for its Lanczos-3 taps and channel replication.
                if (geometry.cropWidth > std::numeric_limits<int>::max() / 32 ||
                    geometry.cropHeight > std::numeric_limits<int>::max() / 32 ||
                    job.input.stride > std::numeric_limits<int>::max()) return false;
                if (job.input.stride < 0) {
                    const auto bytes = std::size_t{geometry.cropWidth} * geometry.cropHeight * job.input.channels;
                    if (packedCrop.size() < bytes) packedCrop.resize(bytes);
                    if (!dispatch(Phase::CopyCrop, geometry.cropHeight)) return false;
                    job.input.pixels = packedCrop.data();
                    job.input.stride = std::int64_t{geometry.cropWidth} * job.input.channels;
                }
                job.preWidth = job.input.quarterTurn ? geometry.outputHeight : geometry.outputWidth;
                job.preHeight = job.input.quarterTurn ? geometry.outputWidth : geometry.outputHeight;
                const bool direct = source.format == PixelFormat::Bgr && geometry.rotation == Rotation::None;
                job.scaled = direct ? destination.data() : scaled.data();
                if (!dispatch(Phase::LancIR, job.preHeight)) return false;
                return direct || dispatch(Phase::Transform, geometry.outputHeight);
            }
        } catch (...) {
            return false;
        }
        return false;
    }
};

std::size_t ScalingExecutor::defaultWorkerCount()
{
    return std::max(1u, std::thread::hardware_concurrency());
}
ScalingExecutor::ScalingExecutor(std::size_t workers) : impl_(std::make_unique<Impl>(workers)) {}
ScalingExecutor::~ScalingExecutor() = default;
bool ScalingExecutor::scale(const SourceFrame& source, const ScalingGeometry& geometry,
                            ScalingAlgorithm algorithm, std::span<std::uint8_t> destination)
{
    return impl_->scale(source, geometry, algorithm, destination);
}
std::size_t ScalingExecutor::workerCount() const { return impl_->count; }
std::size_t ScalingExecutor::threadsStarted() const { return impl_->starts.load(); }
bool ScalingExecutor::isStopped() const { return impl_->stopped.load(); }
void ScalingExecutor::shutdown() { impl_->shutdown(); }
}
