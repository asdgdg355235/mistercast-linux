#include "capture/PipeWireVideoCapture.h"

#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QDebug>

#include <spa/buffer/meta.h>
#include <spa/debug/types.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <utility>
#include <string>
#include <unistd.h>

namespace mistercast {
namespace {

const pw_stream_events kStreamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = PipeWireVideoCapture::onStateChanged;
    events.param_changed = PipeWireVideoCapture::onParamChanged;
    events.process = PipeWireVideoCapture::onProcess;
    return events;
}();

} // namespace

PipeWireVideoCapture::PipeWireVideoCapture(FrameMailbox& mailbox, QObject* parent)
    : QObject(parent)
    , mailbox_(mailbox)
{
    pw_init(nullptr, nullptr);
}

PipeWireVideoCapture::~PipeWireVideoCapture()
{
    stop();
}

bool PipeWireVideoCapture::start(
    int pipeWireFileDescriptor,
    std::uint32_t nodeId,
    std::uint64_t pipeWireSerial,
    QString& error)
{
    Q_ASSERT(thread() == QThread::currentThread());
    stop();

    loop_ = pw_thread_loop_new("mistercast-video", nullptr);
    if (loop_ == nullptr) {
        ::close(pipeWireFileDescriptor);
        error = QStringLiteral("Unable to create the PipeWire video loop");
        return false;
    }

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_ == nullptr) {
        ::close(pipeWireFileDescriptor);
        error = QStringLiteral("Unable to create the PipeWire video context");
        stop();
        return false;
    }

    if (pw_thread_loop_start(loop_) < 0) {
        ::close(pipeWireFileDescriptor);
        error = QStringLiteral("Unable to start the PipeWire video loop");
        stop();
        return false;
    }

    loopStarted_ = true;

    pw_thread_loop_lock(loop_);

    core_ = pw_context_connect_fd(context_, pipeWireFileDescriptor, nullptr, 0);

    if (core_ == nullptr) {
        pw_thread_loop_unlock(loop_);
        error = QStringLiteral("Unable to connect to the portal PipeWire remote");
        stop();
        return false;
    }

    auto* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_NODE_NAME, "mistercast-video-capture",
        nullptr);
    
    if (pipeWireSerial != 0) {
        const auto serial = std::to_string(pipeWireSerial);
        pw_properties_set(properties, PW_KEY_TARGET_OBJECT, serial.c_str());
    }

    stream_ = pw_stream_new(core_, "MiSTerCast screen capture", properties);

    if (stream_ == nullptr) {
        pw_thread_loop_unlock(loop_);
        error = QStringLiteral("Unable to create the PipeWire video stream");
        stop();
        return false;
    }

    pw_stream_add_listener(stream_, &streamListener_, &kStreamEvents, this);

    std::array<std::uint8_t, 2048> parameterBuffer{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(parameterBuffer.data(), parameterBuffer.size());
    
    const auto preferredMode = settingsSnapshot().raster;

    const spa_rectangle preferredSize =
        SPA_RECTANGLE(preferredMode.hActive, preferredMode.vActive);
    const spa_rectangle minimumSize = SPA_RECTANGLE(1, 1);
    const spa_rectangle maximumSize = SPA_RECTANGLE(8192, 8192);
    const spa_fraction preferredFrameRate = SPA_FRACTION(60, 1);
    const spa_fraction minimumFrameRate = SPA_FRACTION(0, 1);
    const spa_fraction maximumFrameRate = SPA_FRACTION(240, 1);
    const spa_pod* parameters[] = {
        static_cast<spa_pod*>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(
                6,
                SPA_VIDEO_FORMAT_BGRx,
                SPA_VIDEO_FORMAT_BGRA,
                SPA_VIDEO_FORMAT_RGBx,
                SPA_VIDEO_FORMAT_RGBA,
                SPA_VIDEO_FORMAT_BGR,
                SPA_VIDEO_FORMAT_RGB),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
                &preferredSize, &minimumSize, &maximumSize),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                &preferredFrameRate, &minimumFrameRate, &maximumFrameRate)))
    };

    running_ = true;
    const int result = pw_stream_connect(
        stream_, PW_DIRECTION_INPUT, pipeWireSerial == 0 ? nodeId : PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        parameters, 1);
    pw_thread_loop_unlock(loop_);

    if (result < 0) {
        running_ = false;
        error = QStringLiteral("Unable to connect the PipeWire video stream: %1")
                    .arg(QString::fromLocal8Bit(spa_strerror(result)));
        stop();

        return false;
    }

    return true;
}

void PipeWireVideoCapture::stop()
{
    Q_ASSERT(thread() == QThread::currentThread());
    running_ = false;

    ++generation_;

    if (loop_ != nullptr && loopStarted_) {
        pw_thread_loop_lock(loop_);
    }

    if (stream_ != nullptr) {
        pw_stream_disconnect(stream_);
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }

    if (core_ != nullptr) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }

    if (loop_ != nullptr && loopStarted_) {
        pw_thread_loop_unlock(loop_);
        pw_thread_loop_stop(loop_);
        loopStarted_ = false;
    }

    if (context_ != nullptr) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }

    if (loop_ != nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }

    format_ = {};

    reachedStreamingState_ = false;
    rerenderPending_ = false;
    rerenderRetryPending_ = false;
    std::lock_guard lock(frameMutex_);
    baseline_.reset();
    lastPublishedRevision_.reset();
    geometryPlan_.reset();
    processingFailures_ = 0;
    mailbox_.invalidate();
}

PipeWireVideoCapture::ProcessingSettings PipeWireVideoCapture::settingsSnapshot()
{
    std::lock_guard lock(settingsMutex_);
    return settings_;
}

void PipeWireVideoCapture::setFramingSettings(FramingSettings settings)
{
    std::lock_guard lock(settingsMutex_);
    settings_.framing = settings;
    ++settings_.framingRevision;
    ++settings_.revision;
}

void PipeWireVideoCapture::setGeometrySettings(CrtGeometrySettings geometry)
{
    Q_ASSERT(thread() == QThread::currentThread());
    {
        std::lock_guard lock(settingsMutex_);
        geometry = clampCrtGeometry(geometry, settings_.raster);
        if (settings_.geometry == geometry) return;
        settings_.geometry = geometry;
        ++settings_.revision;
    }
    scheduleRerender();
}

void PipeWireVideoCapture::setIccSettings(IccProcessingSettings settings)
{
    Q_ASSERT(thread() == QThread::currentThread());
    {
        std::lock_guard lock(settingsMutex_);
        if (settings_.icc == settings) return;
        settings_.icc = std::move(settings);
        ++settings_.revision;
    }
    scheduleRerender();
}

void PipeWireVideoCapture::scheduleRerender(unsigned retries)
{
    Q_ASSERT(thread() == QThread::currentThread());
    // Only the QObject owner can create/destroy the loop or enqueue work.
    // Never take the PipeWire loop lock here, including during a retry.
    if (!running_ || rerenderRetryPending_ || rerenderPending_.exchange(true)) return;
    const auto generation = generation_.load();
    // The pending flag gives this request exclusive use of the token until
    // the callback reads it. Only the wakeup needs to cross the PipeWire queue.
    rerenderGeneration_.store(generation);
    const int result = pw_loop_invoke(pw_thread_loop_get_loop(loop_), onRerender,
        SPA_ID_INVALID, nullptr, 0, false, this);
    if (result >= 0) return;

    rerenderPending_ = false;
    if (!retries) {
        qWarning() << "Unable to queue frame processing update:" << spa_strerror(result);
        return;
    }
    rerenderRetryPending_ = true;
    QTimer::singleShot(4, this, [this, generation, retries] {
        if (generation != generation_.load()) return;
        rerenderRetryPending_ = false;
        scheduleRerender(retries - 1);
    });
}

int PipeWireVideoCapture::onRerender(spa_loop*, bool, std::uint32_t, const void*,
                                    std::size_t, void* userData)
{
    auto& self = *static_cast<PipeWireVideoCapture*>(userData);
    if (self.rerenderGeneration_.load() != self.generation_.load()) return 0;
    self.rerenderPending_ = false;
    if (self.running_) self.rerender();
    return 0;
}

void PipeWireVideoCapture::rerender()
{
    std::lock_guard lock(frameMutex_);
    const auto settings = settingsSnapshot();
    if (!baseline_ || baseline_->epoch != settings.epoch ||
        baseline_->sourceGeneration != sourceGeneration_ ||
        baseline_->framingRevision != settings.framingRevision ||
        lastPublishedRevision_ == settings.revision) return;
    publishConverted(settings, FrameOrigin::GeometryRerender);
}

ModeEpoch PipeWireVideoCapture::setOutputMode(const Modeline& modeline, bool reuseCachedFrame,
                                              CrtGeometrySettings geometry)
{
    Q_ASSERT(thread() == QThread::currentThread());
    std::lock_guard frameLock(frameMutex_);
    ProcessingSettings settings;
    {
        std::lock_guard lock(settingsMutex_);
        settings_.raster.hActive = modeline.hActive;
        settings_.raster.vActive = modeline.vActive;
        settings_.raster.interlaced = modeline.interlaced;
        settings_.geometry = clampCrtGeometry(geometry, modeline);
        ++settings_.epoch.value;
        ++settings_.revision;
        settings = settings_;
    }

    const auto old = std::exchange(baseline_, std::nullopt);
    lastPublishedRevision_.reset();
    if (!reuseCachedFrame || !old || old->framingRevision != settings.framingRevision ||
        old->sourceGeneration != sourceGeneration_ ||
        !validCrtGeometryRaster(modeline)) return settings.epoch;

    if (!old->synthetic) {
        std::copy_n(converted_.begin(), std::size_t{old->width} * old->height * 3,
                    previousConverted_.begin());
        cachedConvertedWidth_ = old->width;
        cachedConvertedHeight_ = old->height;
    }
    const auto startedAt = FrameMailbox::Clock::now();
    if (FrameProcessor::resizeBgr(scalingExecutor_,
            std::span(previousConverted_).first(std::size_t{cachedConvertedWidth_} * cachedConvertedHeight_ * 3),
            cachedConvertedWidth_, cachedConvertedHeight_, converted_,
            modeline.hActive, modeline.vActive, settings.framing.scaling)) {
        baseline_ = {settings.epoch, modeline.hActive, modeline.vActive, sourceGeneration_,
                     settings.framingRevision, old->capturedAt, true};
        publishConverted(settings, FrameOrigin::ModePreview, FrameMailbox::Clock::now() - startedAt);
    } else {
        ++processingFailures_;
    }
    return settings.epoch;
}

void PipeWireVideoCapture::publishConverted(const ProcessingSettings& settings, FrameOrigin origin,
                                             std::chrono::nanoseconds conversion)
{
    if (lastPublishedRevision_ && settings.revision < *lastPublishedRevision_) return;
    const auto& raster = settings.raster;
    std::span<const std::uint8_t> output = std::span(converted_).first(raster.fullFrameBytes());
    std::chrono::nanoseconds geometryTime{};
    if (settings.geometry.effective()) {
        const auto startedAt = FrameMailbox::Clock::now();
        if (!geometryPlan_ || plannedEpoch_ != settings.epoch || plannedGeometry_ != settings.geometry) {
            geometryPlan_ = CrtGeometryPlan::make(raster, settings.geometry);
            plannedEpoch_ = settings.epoch;
            plannedGeometry_ = settings.geometry;
        }
        if (!geometryPlan_ || !scalingExecutor_.applyGeometry(*geometryPlan_, output, geometryOutput_)) {
            if (++processingFailures_ == 1) qWarning() << "CRT geometry render failed";
            return;
        }
        output = std::span(geometryOutput_).first(raster.fullFrameBytes());
        geometryTime = FrameMailbox::Clock::now() - startedAt;
    }
    std::chrono::nanoseconds iccTime{};
    if (settings.icc.transform) {
        const auto startedAt = FrameMailbox::Clock::now();
        if (!iccOutput_) iccOutput_.reset(new (std::nothrow) std::uint8_t[kMaximumFrameBytes]);
        if (!iccOutput_ || !scalingExecutor_.applyIcc(*settings.icc.transform, output,
                std::span(iccOutput_.get(), raster.fullFrameBytes()), raster.hActive, raster.vActive,
                settings.icc.applyCalibration)) {
            if (++processingFailures_ == 1) qWarning() << "ICC frame conversion failed";
            return;
        }
        output = std::span(iccOutput_.get(), raster.fullFrameBytes());
        iccTime = FrameMailbox::Clock::now() - startedAt;
    }
    mailbox_.publish(output, raster.hActive, raster.vActive, settings.epoch, baseline_->capturedAt,
                     {.origin = origin, .settingsRevision = settings.revision, .conversion = conversion,
                      .geometry = geometryTime, .failures = processingFailures_, .icc = iccTime});
    lastPublishedRevision_ = settings.revision;
}

void PipeWireVideoCapture::convertSource(const SourceFrame* source,
                                         FrameMailbox::Clock::time_point capturedAt)
{
    std::lock_guard lock(frameMutex_);
    const auto settings = settingsSnapshot();
    const auto& mode = settings.raster;
    const auto startedAt = FrameMailbox::Clock::now();
    // Invalidate before writing. A worker can stop after writing a partial raster.
    baseline_.reset();
    if (!validCrtGeometryRaster(mode)) {
        ++processingFailures_;
        return;
    }
    if (!source) {
        std::fill_n(converted_.begin(), mode.fullFrameBytes(), 0);
    } else if (!FrameProcessor::convertToBgr(
            scalingExecutor_, *source, converted_, mode.hActive, mode.vActive, settings.framing)) {
        ++processingFailures_;
        return;
    }
    baseline_ = {settings.epoch, mode.hActive, mode.vActive, sourceGeneration_,
                 settings.framingRevision, capturedAt, false};
    publishConverted(settings, FrameOrigin::Capture, FrameMailbox::Clock::now() - startedAt);
}

void PipeWireVideoCapture::onStateChanged(
    void* data,
    pw_stream_state,
    pw_stream_state state,
    const char* error)
{
    static_cast<PipeWireVideoCapture*>(data)->handleStateChanged(state, error);
}

void PipeWireVideoCapture::onParamChanged(
    void* data,
    std::uint32_t id,
    const spa_pod* parameter)
{
    static_cast<PipeWireVideoCapture*>(data)->handleParamChanged(id, parameter);
}

void PipeWireVideoCapture::onProcess(void* data)
{
    static_cast<PipeWireVideoCapture*>(data)->processFrame();
}

void PipeWireVideoCapture::handleStateChanged(pw_stream_state state, const char* error)
{
    if (state == PW_STREAM_STATE_STREAMING) {
        reachedStreamingState_ = true;
        return;
    }

    if (!running_) {
        return;
    }

    if (state == PW_STREAM_STATE_ERROR ||
        (state == PW_STREAM_STATE_UNCONNECTED && reachedStreamingState_)) {
        const QString message = QStringLiteral("PipeWire video stream failed: %1")
                                    .arg(QString::fromLocal8Bit(error == nullptr ? "unknown error" : error));
        const auto generation = generation_.load();
        QMetaObject::invokeMethod(this, [this, message, generation] {
            if (generation == generation_.load()) {
                emit failed(message);
            }
        });
    }
}

void PipeWireVideoCapture::handleParamChanged(std::uint32_t id, const spa_pod* parameter)
{
    if (id != SPA_PARAM_Format) return;
    {
        std::lock_guard lock(frameMutex_);
        ++sourceGeneration_;
        baseline_.reset();
    }
    format_ = {};
    if (!parameter) return;

    spa_video_info_raw parsed{};
    if (spa_format_video_raw_parse(parameter, &parsed) < 0) {
        return;
    }

    PixelFormat ignored;
    if (!pixelFormat(parsed.format, ignored)) {
        const QString message = QStringLiteral("The compositor selected an unsupported pixel format");
        const auto generation = generation_.load();

        QMetaObject::invokeMethod(this, [this, message, generation] {
            if (generation == generation_.load()) {
                emit failed(message);
            }
        });

        return;
    }

    format_ = parsed;
    qDebug() << "PipeWire capture color range/transfer:" << parsed.color_range << parsed.transfer_function;

    std::array<std::uint8_t, 1024> parameterBuffer{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(parameterBuffer.data(), parameterBuffer.size());
    const std::uint32_t dataTypes = (1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd);
    const spa_pod* parameters[] = {
        static_cast<spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(3, 2, 4),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(
                static_cast<int>(parsed.size.width * parsed.size.height * 4)),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(static_cast<int>(parsed.size.width * 4)),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(static_cast<int>(dataTypes)))),
        static_cast<spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)))),
    };

    pw_stream_update_params(stream_, parameters, 2);

    const auto width = parsed.size.width;
    const auto height = parsed.size.height;
    const auto name = formatName(parsed.format);
    const auto generation = generation_.load();

    QMetaObject::invokeMethod(this, [this, width, height, name, generation] {
        if (generation == generation_.load()) {
            emit started(width, height, name);
        }
    });
}

void PipeWireVideoCapture::processFrame()
{
    pw_buffer* latest = nullptr;

    for (;;) {
        pw_buffer* candidate = pw_stream_dequeue_buffer(stream_);
        
        if (candidate == nullptr) {
            break;
        }

        if (latest != nullptr) {
            pw_stream_queue_buffer(stream_, latest);
        }

        latest = candidate;
    }

    if (latest == nullptr) {
        return;
    }

    const auto dequeuedAt = FrameMailbox::Clock::now();
    const spa_buffer* buffer = latest->buffer;

    if (buffer->n_datas == 0 || buffer->datas[0].data == nullptr ||
        buffer->datas[0].chunk == nullptr || buffer->datas[0].chunk->size == 0 ||
        (buffer->datas[0].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0) {
        pw_stream_queue_buffer(stream_, latest);
        return;
    }

    if ((buffer->datas[0].chunk->flags & SPA_CHUNK_FLAG_EMPTY) != 0) {
        convertSource(nullptr, dequeuedAt);
        pw_stream_queue_buffer(stream_, latest);
        return;
    }

    PixelFormat pixelFormatValue;
    if (!format_.size.width || !format_.size.height || !pixelFormat(format_.format, pixelFormatValue)) {
        pw_stream_queue_buffer(stream_, latest);
        return;
    }

    const auto& data = buffer->datas[0];
    if (data.maxsize == 0) {
        pw_stream_queue_buffer(stream_, latest);
        return;
    }

    const std::uint32_t bytesPerPixel =
        pixelFormatValue == PixelFormat::Bgr || pixelFormatValue == PixelFormat::Rgb ? 3 : 4;
    const std::int64_t rowBytes = static_cast<std::int64_t>(format_.size.width) * bytesPerPixel;
    const std::int64_t stride = data.chunk->stride == 0 ? rowBytes : data.chunk->stride;
    const std::int64_t absoluteStride = stride < 0 ? -stride : stride;
    const std::size_t offset = data.chunk->offset % data.maxsize;
    const std::size_t contiguousBytes = std::min<std::size_t>(
        data.chunk->size, static_cast<std::size_t>(data.maxsize) - offset);

    bool validRange = absoluteStride >= rowBytes;

    if (stride >= 0) {
        const auto required = static_cast<std::uint64_t>(absoluteStride) *
                (format_.size.height - 1) +
            static_cast<std::uint64_t>(rowBytes);
        validRange = validRange && required <= contiguousBytes;
    } else {
        const auto requiredBefore = static_cast<std::uint64_t>(absoluteStride) *
            (format_.size.height - 1);
        validRange = validRange && requiredBefore <= offset &&
            static_cast<std::uint64_t>(rowBytes) <= data.maxsize - offset &&
            requiredBefore + static_cast<std::uint64_t>(rowBytes) <= data.chunk->size;
    }

    if (!validRange) {
        pw_stream_queue_buffer(stream_, latest);
        return;
    }

    const auto* pixels = static_cast<const std::uint8_t*>(data.data) + offset;
    const SourceFrame source{
        pixels,
        format_.size.width,
        format_.size.height,
        data.chunk->stride,
        pixelFormatValue,
    };

    convertSource(&source, dequeuedAt);
    pw_stream_queue_buffer(stream_, latest);
}

bool PipeWireVideoCapture::pixelFormat(spa_video_format format, PixelFormat& output)
{
    switch (format) {
    case SPA_VIDEO_FORMAT_BGR:
        output = PixelFormat::Bgr;
        return true;
    case SPA_VIDEO_FORMAT_BGRx:
        output = PixelFormat::Bgrx;
        return true;
    case SPA_VIDEO_FORMAT_BGRA:
        output = PixelFormat::Bgra;
        return true;
    case SPA_VIDEO_FORMAT_RGB:
        output = PixelFormat::Rgb;
        return true;
    case SPA_VIDEO_FORMAT_RGBx:
        output = PixelFormat::Rgbx;
        return true;
    case SPA_VIDEO_FORMAT_RGBA:
        output = PixelFormat::Rgba;
        return true;
    default:
        return false;
    }
}

QString PipeWireVideoCapture::formatName(spa_video_format format)
{
    const char* name = spa_debug_type_find_name(spa_type_video_format, format);
    return QString::fromLatin1(name == nullptr ? "unknown" : name);
}

} // namespace mistercast
