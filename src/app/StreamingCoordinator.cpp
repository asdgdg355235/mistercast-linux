#include "app/StreamingCoordinator.h"

namespace mistercast {

StreamingCoordinator::StreamingCoordinator(QObject* parent)
    : QObject(parent)
    , audioCapture_(this)
    , videoCapture_(mailbox_, this)
    , portal_(this)
    , controller_(mailbox_, audioCapture_, this)
{
    connect(&portal_, &ScreenCastPortal::streamReady, this,
        [this](int descriptor, std::uint32_t nodeId, std::uint64_t pipeWireSerial) {
            QString error;
            if (!videoCapture_.start(descriptor, nodeId, pipeWireSerial, error)) {
                sourceReady_ = false;
                videoCapture_.stop();
                portal_.stop();
                emit failed(error);
                emit stateChanged();
            }
        });

    connect(&portal_, &ScreenCastPortal::cancelled,
        this, &StreamingCoordinator::sourceSelectionCancelled);

    connect(&portal_, &ScreenCastPortal::failed, this,
        [this](const QString& message) {
            sourceReady_ = false;
            stopStreaming();
            videoCapture_.stop();
            emit failed(message);
            emit stateChanged();
        });

    connect(&videoCapture_, &PipeWireVideoCapture::started, this,
        [this](std::uint32_t width, std::uint32_t height, const QString& format) {
            const CrtSourceSize size{width, height};
            const bool changed = sourceSize_ != size;
            sourceSize_ = size;
            sourceReady_ = true;
            emit sourceStarted(width, height, format);
            if (automaticMode_ && changed) {
                resolveOutput(true);
            }
            emit stateChanged();
        });

    connect(&videoCapture_, &PipeWireVideoCapture::failed, this,
        [this](const QString& message) {
            sourceReady_ = false;
            stopStreaming();
            videoCapture_.stop();
            portal_.stop();
            emit failed(message);
            emit stateChanged();
        });

    connect(&audioCapture_, &PipeWireAudioCapture::failed, this,
        [this](const QString& message) {
            stopStreaming();
            emit failed(message);
        });

    connect(&controller_, &StreamController::started, this,
        [this] {
            emit streamStarted();
            emit stateChanged();
        });

    connect(&controller_, &StreamController::stopped, this,
        [this] {
            if (controller_.running()) {
                return;
            }
            audioCapture_.stop();
            activeConfig_.reset();
            emit streamStopped();
            emit stateChanged();
        });

    connect(&controller_, &StreamController::failed,
        this, &StreamingCoordinator::failed);

    connect(&controller_, &StreamController::warning,
        this, &StreamingCoordinator::warning);

    connect(&controller_, &StreamController::videoDiagnostics,
        this, &StreamingCoordinator::videoDiagnostics);

    connect(&controller_, &StreamController::audioDiagnostics,
        this, &StreamingCoordinator::audioDiagnostics);

    connect(&controller_, &StreamController::fpgaDiagnostics,
        this, &StreamingCoordinator::fpgaDiagnostics);
}

StreamingCoordinator::~StreamingCoordinator()
{
    shutdown();
}

void StreamingCoordinator::selectSource()
{
    if (controller_.running()) {
        return;
    }

    sourceReady_ = false;
    sourceSize_ = {};

    videoCapture_.stop();
    if (automaticMode_) {
        resolveOutput(true);
    }
    emit sourceSelectionStarted();
    emit stateChanged();
    portal_.selectSource();
}

void StreamingCoordinator::startStreaming(std::string host, bool audioEnabled,
                                          bool controllerEnabled, std::string mpvIpcPath, bool systemGamepads)
{
    if (!sourceReady_ || controller_.running() || !resolveOutput()) {
        return;
    }

    beginStreaming({
        std::move(host), audioEnabled, outputMode_, modeEpoch_,
        controllerEnabled, std::move(mpvIpcPath), systemGamepads,
    });
}

void StreamingCoordinator::beginStreaming(StreamConfig config)
{
    if (config.audioEnabled) {
        QString error;
        if (!audioCapture_.start(error)) {
            emit failed(error);
            return;
        }
    }

    config.modeline = outputMode_;
    config.modeEpoch = modeEpoch_;
    activeConfig_ = config;
    emit streamConnecting(config.audioEnabled);

    if (!controller_.start(std::move(config))) {
        audioCapture_.stop();
        activeConfig_.reset();
        emit failed(QStringLiteral("Unable to start the stream worker"));
        emit streamStopped();
    }

    emit stateChanged();
}

void StreamingCoordinator::stopStreaming()
{
    const bool wasStreaming = controller_.running();

    controller_.stop();
    audioCapture_.stop();
    activeConfig_.reset();

    if (wasStreaming) {
        emit streamStopped();
        emit stateChanged();
    }
}

void StreamingCoordinator::shutdown()
{
    stopStreaming();

    videoCapture_.stop();
    portal_.stop();

    sourceReady_ = false;
}

void StreamingCoordinator::configureOutput(
    const Modeline& manual, std::optional<CrtRequest> automatic, const std::vector<Modeline>& catalog)
{
    manualMode_ = manual;
    automaticMode_ = std::move(automatic);
    modelines_ = catalog;
    resolveOutput();
}

bool StreamingCoordinator::resolveOutput(bool sourceChanged)
{
    auto result = selectCrtMode(automaticMode_, manualMode_, modelines_, sourceSize_);
    for (const auto& message : result.diagnostics) {
        emit warning(QString::fromStdString(message));
    }
    if (!result.mode) {
        stopStreaming();
        const auto error = QString::fromStdString(result.error);
        emit outputModePreview(error);
        emit failed(error);
        return false;
    }

    const auto& mode = *result.mode;
    const auto label = result.selection == CrtSelection::Fallback
        ? QStringLiteral("Fallback · %1").arg(QString::fromStdString(mode.name))
        : QString::fromStdString(mode.name);
    const auto details = QStringLiteral(
        "%1 MHz  |  %2 kHz H  |  %3 Hz %4\n"
        "Active / sync start / sync end / total\nH %5 / %6 / %7 / %8  |  V %9 / %10 / %11 / %12")
        .arg(mode.pixelClockMHz, 0, 'f', 6)
        .arg(mode.horizontalHz() / 1000.0, 0, 'f', 5)
        .arg(mode.cadenceHz(), 0, 'f', 5)
        .arg(mode.interlaced ? QStringLiteral("interlaced field rate") : QStringLiteral("progressive"))
        .arg(mode.hActive).arg(mode.hBegin).arg(mode.hEnd).arg(mode.hTotal)
        .arg(mode.vActive).arg(mode.vBegin).arg(mode.vEnd).arg(mode.vTotal);
    emit outputModePreview(label + QStringLiteral("  |  ") + details);

    if (mode == outputMode_) {
        return true;
    }

    // Join the old sender before changing the capture epoch. The next session
    // creates fresh encoder, field, and pacing state from the same modeline.
    auto restart = controller_.running() ? activeConfig_ : std::nullopt;
    if (restart) {
        emit warning(QStringLiteral("Restarting the stream for %1").arg(label));
        stopStreaming();
    }
    outputMode_ = mode;
    modeEpoch_ = videoCapture_.setOutputMode(mode, !sourceChanged);
    if (restart) {
        beginStreaming(std::move(*restart));
    }
    return true;
}

void StreamingCoordinator::setFramingSettings(FramingSettings settings)
{
    videoCapture_.setFramingSettings(settings);
}

} // namespace mistercast
