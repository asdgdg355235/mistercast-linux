#pragma once

#include "audio/PipeWireAudioCapture.h"
#include "capture/PipeWireVideoCapture.h"
#include "capture/ScreenCastPortal.h"
#include "core/CrtModeline.h"
#include "core/FrameMailbox.h"
#include "core/FrameProcessor.h"
#include "stream/StreamController.h"
#include "stream/StreamDiagnostics.h"

#include <QObject>

#include <cstdint>
#include <string>

namespace mistercast {

class StreamingCoordinator final : public QObject {
    Q_OBJECT

public:
    explicit StreamingCoordinator(QObject* parent = nullptr);
    ~StreamingCoordinator() override;

    void selectSource();
    void startStreaming(std::string host, bool audioEnabled,
                        bool controllerEnabled = false, std::string mpvIpcPath = {},
                        bool systemGamepads = false);
    void stopStreaming();
    void setGamepadSlots(unsigned mask) { controller_.setGamepadSlots(mask); }
    void shutdown();
    void configureOutput(const Modeline& manual, std::optional<CrtRequest> automatic,
                         const std::vector<Modeline>& catalog);
    void setFramingSettings(FramingSettings settings);

    [[nodiscard]] bool sourceReady() const { return sourceReady_; }
    [[nodiscard]] bool streaming() const { return controller_.running(); }

signals:
    void sourceSelectionStarted();
    void sourceSelectionCancelled();
    void sourceStarted(
        std::uint32_t width,
        std::uint32_t height,
        const QString& format);
    void streamConnecting(bool audioEnabled);
    void streamStarted();
    void streamStopped();
    void stateChanged();
    void outputModePreview(const QString& description);
    void failed(const QString& message);
    void warning(const QString& message);
    void videoDiagnostics(const VideoDiagnostics& snapshot);
    void audioDiagnostics(const AudioDiagnostics& snapshot);
    void fpgaDiagnostics(const FpgaDiagnostics& snapshot);

private:
    bool resolveOutput(bool sourceChanged = false);
    void beginStreaming(StreamConfig config);

    FrameMailbox mailbox_;
    PipeWireAudioCapture audioCapture_;
    PipeWireVideoCapture videoCapture_;
    ScreenCastPortal portal_;
    StreamController controller_;
    Modeline manualMode_{kDefaultModeline};
    std::optional<CrtRequest> automaticMode_;
    std::vector<Modeline> modelines_;
    CrtSourceSize sourceSize_;
    std::optional<StreamConfig> activeConfig_;
    Modeline outputMode_{kDefaultModeline};
    ModeEpoch modeEpoch_{1};
    bool sourceReady_{};
};

} // namespace mistercast
