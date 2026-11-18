#pragma once

#include "core/CrtGeometry.h"
#include "core/FrameMailbox.h"
#include "core/FrameProcessor.h"
#include "core/IccTransform.h"
#include "core/ScalingExecutor.h"
#include "core/Modeline.h"

#include <QObject>

#include <pipewire/pipewire.h>
#include <spa/param/video/raw.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace mistercast {

class PipeWireVideoCapture final : public QObject {
    Q_OBJECT

public:
    explicit PipeWireVideoCapture(FrameMailbox& mailbox, QObject* parent = nullptr);
    ~PipeWireVideoCapture() override;

    bool start(
        int pipeWireFileDescriptor,
        std::uint32_t nodeId,
        std::uint64_t pipeWireSerial,
        QString& error);
    void stop();
    void setContentViewport(ContentViewport viewport);
    void setFramingSettings(FramingSettings settings);
    void setIccSettings(IccProcessingSettings settings);
    void setGeometrySettings(CrtGeometrySettings settings);
    ModeEpoch setOutputMode(const Modeline& modeline, bool reuseCachedFrame,
                            CrtGeometrySettings geometry = {}, ContentViewport viewport = {});

signals:
    void started(std::uint32_t width, std::uint32_t height, const QString& format);
    void failed(const QString& message);

public:
    static void onStateChanged(
        void* data, pw_stream_state oldState, pw_stream_state state, const char* error);
    static void onParamChanged(void* data, std::uint32_t id, const spa_pod* parameter);
    static void onProcess(void* data);

private:
    void handleStateChanged(pw_stream_state state, const char* error);
    void handleParamChanged(std::uint32_t id, const spa_pod* parameter);
    void processFrame();
    static bool pixelFormat(spa_video_format format, PixelFormat& output);
    static QString formatName(spa_video_format format);

    FrameMailbox& mailbox_;
    pw_thread_loop* loop_{};
    pw_context* context_{};
    pw_core* core_{};
    pw_stream* stream_{};
    spa_hook streamListener_{};
    spa_video_info_raw format_{};
    struct ProcessingSettings {
        // Only raster dimensions and scan type are needed by the capture path.
        Modeline raster{.name = {}, .hActive = kDefaultModeline.hActive, .vActive = kDefaultModeline.vActive};
        ModeEpoch epoch{1};
        FramingSettings framing{};
        ContentViewport viewport{};
        CrtGeometrySettings geometry{};
        IccProcessingSettings icc{};
        std::uint64_t revision{}, framingRevision{};
    };
    ProcessingSettings settings_;
    std::mutex settingsMutex_;
    ProcessingSettings settingsSnapshot();
    void scheduleRerender(unsigned retries = 2);
    static int onRerender(spa_loop*, bool, std::uint32_t, const void*, std::size_t, void*);
    void rerender();
    void convertSource(const SourceFrame* source, FrameMailbox::Clock::time_point capturedAt);
    bool renderBaseline(const SourceFrame* source, const ProcessingSettings& settings,
                        FrameMailbox::Clock::time_point capturedAt);
    void cacheSource(const SourceFrame& source);
    void publishConverted(const ProcessingSettings& settings, FrameOrigin origin,
                          std::chrono::nanoseconds conversion = {});

    // GUI owns start/stop/scheduling. Capture callbacks and mode changes
    // serialize baseline ownership under frameMutex_ before taking settingsMutex_.
    friend class PipeWireGeometryTest;
    std::array<std::uint8_t, kMaximumFrameBytes> converted_{};
    std::array<std::uint8_t, kMaximumFrameBytes> viewportScratch_;
    std::unique_ptr<std::uint8_t[]> sourceStorage_;
    std::size_t sourceCapacity_{};
    std::optional<SourceFrame> cachedSource_;
    bool cachedBlank_{};
    std::array<std::uint8_t, kMaximumFrameBytes> geometryOutput_;
    std::unique_ptr<std::uint8_t[]> iccOutput_;
    ScalingExecutor scalingExecutor_;
    std::mutex frameMutex_;
    struct Baseline {
        ModeEpoch epoch;
        std::uint64_t sourceGeneration, framingRevision;
        FrameMailbox::Clock::time_point capturedAt;
    };
    std::optional<Baseline> baseline_;
    std::uint64_t sourceGeneration_{}, processingFailures_{};
    std::optional<std::uint64_t> lastPublishedRevision_;
    std::optional<CrtGeometryPlan> geometryPlan_;
    CrtGeometrySettings plannedGeometry_{};
    ModeEpoch plannedEpoch_{};
    std::atomic_bool rerenderPending_{};
    std::atomic_uint64_t rerenderGeneration_{};
    bool rerenderRetryPending_{}; // GUI thread
    std::atomic_bool running_{};
    std::atomic_uint64_t generation_{};
    bool loopStarted_{};
    bool reachedStreamingState_{};
};

} // namespace mistercast
