#include "ui/MainWindow.h"
#include "ui/CrtModeControls.h"
#include "ui/CrtGeometryControls.h"
#include "ui/ScalingSelector.h"
#include "ui/IccProfileControls.h"

#include "core/ModelineCatalog.h"
#include "app/CrtGeometryProfile.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <filesystem>

namespace mistercast {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , streaming_(this)
{
    setWindowTitle(QStringLiteral("MiSTerCast"));
    resize(660, 620);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(8, 8, 8, 8);
    auto* settingsScroll = new QScrollArea(central);
    settingsScroll->setObjectName(QStringLiteral("settingsScroll"));
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto* settingsPanel = new QWidget;
    auto* settingsLayout = new QVBoxLayout(settingsPanel);
    settingsLayout->setContentsMargins(6, 6, 6, 6);
    settingsScroll->setWidget(settingsPanel);
    layout->addWidget(settingsScroll, 5);

    auto* connectionGroup = new QGroupBox(QStringLiteral("Connection"), central);
    auto* form = new QFormLayout(connectionGroup);
    hostEdit_ = new QLineEdit(connectionGroup);
    hostEdit_->setPlaceholderText(QStringLiteral("192.168.1.50"));
    QSettings settings;
    hostEdit_->setText(settings.value(QStringLiteral("misterHost"), QStringLiteral("192.168.1.50")).toString());
    form->addRow(QStringLiteral("MiSTer address"), hostEdit_);
    audioCheck_ = new QCheckBox(QStringLiteral("Capture default output audio (48 kHz stereo)"), connectionGroup);
    audioCheck_->setChecked(settings.value(QStringLiteral("audio"), true).toBool());
    form->addRow(QString(), audioCheck_);
    gamepadCheck_ = new QCheckBox(QStringLiteral("Forward MiSTer controllers as Linux gamepads"), connectionGroup);
    gamepadCheck_->setChecked(settings.value(QStringLiteral("systemGamepads"), false).toBool());
    gamepadCheck_->setToolTip(QStringLiteral("System-wide uinput gamepads during streaming. Replaces direct mpv mappings."));
    form->addRow(QString(), gamepadCheck_);
    gamepadSlots_ = new QComboBox(connectionGroup);
    gamepadSlots_->addItem(QStringLiteral("Both"), 3);
    gamepadSlots_->addItem(QStringLiteral("Controller 1"), 1);
    gamepadSlots_->addItem(QStringLiteral("Controller 2"), 2);
    const auto savedSlots = settings.value(QStringLiteral("gamepadSlots"), 3).toInt();
    const auto savedIndex = gamepadSlots_->findData(savedSlots);
    gamepadSlots_->setCurrentIndex(savedIndex < 0 ? 0 : savedIndex);
    gamepadSlots_->setEnabled(gamepadCheck_->isChecked());
    form->addRow(QStringLiteral("Active Linux gamepads"), gamepadSlots_);
    const auto applySlots = [this] {
        streaming_.setGamepadSlots(gamepadSlots_->currentData().toUInt());
    };
    connect(gamepadSlots_, &QComboBox::currentIndexChanged, this, applySlots);
    applySlots();
    controllerCheck_ = new QCheckBox(QStringLiteral("Control mpv with MiSTer controllers"), connectionGroup);
    controllerCheck_->setChecked(settings.value(QStringLiteral("controllerEnabled"), false).toBool());
    form->addRow(QString(), controllerCheck_);
    if (gamepadCheck_->isChecked()) controllerCheck_->setChecked(false);
    connect(gamepadCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        gamepadSlots_->setEnabled(enabled);
        if (enabled) controllerCheck_->setChecked(false);
    });
    connect(controllerCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) gamepadCheck_->setChecked(false);
    });
    mpvIpcEdit_ = new QLineEdit(connectionGroup);
    mpvIpcEdit_->setPlaceholderText(QStringLiteral("Absolute path passed to mpv --input-ipc-server"));
    mpvIpcEdit_->setText(settings.value(QStringLiteral("mpvIpcPath")).toString());
    form->addRow(QStringLiteral("mpv IPC socket"), mpvIpcEdit_);
    new IccProfileControls(*form, [this](IccProcessingSettings icc) {
        streaming_.setIccSettings(std::move(icc));
    });
    diagnosticsCheck_ = new QCheckBox(QStringLiteral("Show diagnostics"), connectionGroup);
    diagnosticsCheck_->setChecked(settings.value(QStringLiteral("diagnostics"), false).toBool());
    diagnosticsCheck_->setObjectName(QStringLiteral("showDiagnostics"));
    form->addRow(QString(), diagnosticsCheck_);
    settingsLayout->addWidget(connectionGroup);

    auto* modeGroup = new QGroupBox(QStringLiteral("CRT Output"), central);
    auto* modeLayout = new QVBoxLayout(modeGroup);
    crtModeControls_ = new CrtModeControls(modeGroup);
    modeLayout->addWidget(crtModeControls_);
    auto* modeRow = new QHBoxLayout;
    modelineSelect_ = new QComboBox(modeGroup);
    editModelinesButton_ = new QPushButton(QStringLiteral("Edit File"), modeGroup);
    reloadModelinesButton_ = new QPushButton(QStringLiteral("Reload"), modeGroup);
    modeRow->addWidget(modelineSelect_, 1);
    modeRow->addWidget(editModelinesButton_);
    modeRow->addWidget(reloadModelinesButton_);
    modeLayout->addLayout(modeRow);
    timingDetails_ = new QLabel(modeGroup);
    timingDetails_->setTextFormat(Qt::PlainText);
    timingDetails_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    timingDetails_->setWordWrap(true);
    timingDetails_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    timingDetails_->setObjectName(QStringLiteral("timingDetails"));
    modeLayout->addWidget(timingDetails_);
    settingsLayout->addWidget(modeGroup);
    auto* geometry = new CrtGeometryControls(central);
    settingsLayout->addWidget(geometry);
    connect(geometry, &CrtGeometryControls::geometryChanged,
            &streaming_, &StreamingCoordinator::setGeometrySettings);
    connect(&streaming_, &StreamingCoordinator::outputModeChanged,
            geometry, &CrtGeometryControls::setMode);

    auto* framingGroup = new QGroupBox(QStringLiteral("Framing"), central);
    auto* framingForm = new QFormLayout(framingGroup);
    horizontalAlignment_ = new QComboBox(framingGroup);
    horizontalAlignment_->addItems({QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right")});
    horizontalAlignment_->setCurrentIndex(settings.value(QStringLiteral("horizontalAlignment"), 1).toInt());
    verticalAlignment_ = new QComboBox(framingGroup);
    verticalAlignment_->addItems({QStringLiteral("Top"), QStringLiteral("Center"), QStringLiteral("Bottom")});
    verticalAlignment_->setCurrentIndex(settings.value(QStringLiteral("verticalAlignment"), 1).toInt());
    rotation_ = new QComboBox(framingGroup);
    rotation_->addItems({QStringLiteral("None"), QStringLiteral("90 degrees clockwise"),
                         QStringLiteral("90 degrees counter-clockwise"), QStringLiteral("180 degrees")});
    rotation_->setCurrentIndex(settings.value(QStringLiteral("rotation"), 0).toInt());
    scaling_ = new QComboBox(framingGroup);
    populateScalingSelector(*scaling_, settings);
    horizontalOffset_ = new QSpinBox(framingGroup);
    horizontalOffset_->setRange(-4096, 4096);
    horizontalOffset_->setValue(settings.value(QStringLiteral("horizontalOffset"), 0).toInt());
    verticalOffset_ = new QSpinBox(framingGroup);
    verticalOffset_->setRange(-4096, 4096);
    verticalOffset_->setValue(settings.value(QStringLiteral("verticalOffset"), 0).toInt());
    framingForm->addRow(QStringLiteral("Horizontal source alignment"), horizontalAlignment_);
    framingForm->addRow(QStringLiteral("Vertical source alignment"), verticalAlignment_);
    framingForm->addRow(QStringLiteral("Rotation"), rotation_);
    framingForm->addRow(QStringLiteral("Scaling"), scaling_);
    framingForm->addRow(QStringLiteral("Horizontal source offset"), horizontalOffset_);
    framingForm->addRow(QStringLiteral("Vertical source offset"), verticalOffset_);
    auto* viewportTitle = new QLabel(tr("Content viewport"), framingGroup);
    framingForm->addRow(viewportTitle);
    customContent_ = new QCheckBox(tr("Custom content dimensions"), framingGroup);
    centerContent_ = new QCheckBox(tr("Center content"), framingGroup);
    customContent_->setObjectName(QStringLiteral("customContent"));
    framingForm->addRow(customContent_);
    const auto contentControl = [&](const QString& label, const QString& name, const QString& tip) {
        auto* control = new QSpinBox(framingGroup);
        control->setObjectName(name);
        control->setToolTip(tip);
        control->setKeyboardTracking(false);
        framingForm->addRow(label, control);
        return control;
    };
    contentWidth_ = contentControl(tr("Content width"), QStringLiteral("contentWidth"),
                                   tr("Picture width in output pixels."));
    contentHeight_ = contentControl(tr("Content height"), QStringLiteral("contentHeight"),
                                    tr("Picture height in full-frame output lines."));
    framingForm->addRow(centerContent_);
    contentX_ = contentControl(tr("Content X"), QStringLiteral("contentX"),
                                tr("Horizontal origin in pixels from the left edge. Negative values clip."));
    contentY_ = contentControl(tr("Content Y"), QStringLiteral("contentY"),
                                tr("Vertical origin in full-frame lines from the top edge. Negative values clip."));
    rasterSummary_ = new QLabel(framingGroup);
    viewportSummary_ = new QLabel(framingGroup);
    viewportSummary_->setWordWrap(true);
    framingForm->addRow(tr("Output raster"), rasterSummary_);
    framingForm->addRow(tr("Visible content"), viewportSummary_);
    auto* resetViewport = new QPushButton(tr("Reset viewport"), framingGroup);
    framingForm->addRow(QString(), resetViewport);

    connect(customContent_, &QCheckBox::toggled, this, [this](bool value) {
        viewport_.enabled = value;
        saveViewport();
    });
    connect(centerContent_, &QCheckBox::toggled, this, [this](bool value) {
        viewport_.centered = value;
        saveViewport();
    });
    const auto connectCoordinate = [&](QSpinBox* spin, int ContentViewport::*field) {
        connect(spin, &QSpinBox::valueChanged, this, [this, field](int value) {
            viewport_.*field = value;
            saveViewport();
        });
    };
    connectCoordinate(contentWidth_, &ContentViewport::width);
    connectCoordinate(contentHeight_, &ContentViewport::height);
    connectCoordinate(contentX_, &ContentViewport::x);
    connectCoordinate(contentY_, &ContentViewport::y);
    connect(resetViewport, &QPushButton::clicked, this, [this] {
        viewport_ = {};
        saveViewport();
    });
    connect(&streaming_, &StreamingCoordinator::outputModeChanged, this, &MainWindow::restoreViewport);
    restoreViewport(streaming_.outputMode());
    settingsLayout->addWidget(framingGroup);
    settingsLayout->addStretch();
    for (auto* combo : settingsPanel->findChildren<QComboBox*>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(8);
    }

    auto* actions = new QHBoxLayout;
    sourceButton_ = new QPushButton(QStringLiteral("Select Desktop Source"), central);
    startButton_ = new QPushButton(QStringLiteral("Start Streaming"), central);
    stopButton_ = new QPushButton(QStringLiteral("Stop"), central);
    sourceButton_->setObjectName(QStringLiteral("selectSource"));
    startButton_->setObjectName(QStringLiteral("startStreaming"));
    stopButton_->setObjectName(QStringLiteral("stopStreaming"));
    actions->addWidget(sourceButton_);
    actions->addWidget(startButton_);
    actions->addWidget(stopButton_);
    layout->addLayout(actions);

    auto* statusScroll = new QScrollArea(central);
    statusScroll->setObjectName(QStringLiteral("statusScroll"));
    statusScroll->setWidgetResizable(true);
    statusScroll->setMinimumHeight(54);
    statusScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    auto* statusPanel = new QWidget;
    auto* statusLayout = new QVBoxLayout(statusPanel);
    statusLayout->setContentsMargins(5, 2, 5, 2);
    const auto statusLabel = [&](const QString& message, const QString& name) {
        auto* label = new QLabel(message, statusPanel);
        label->setObjectName(name);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        statusLayout->addWidget(label);
        return label;
    };
    sourceStatus_ = statusLabel(tr("Source: not selected"), QStringLiteral("sourceStatus"));
    streamStatus_ = statusLabel(tr("Stream: idle"), QStringLiteral("streamStatus"));
    latencyStatus_ = statusLabel(tr("Latency metrics: waiting"), QStringLiteral("latencyStatus"));
    audioStatus_ = statusLabel(tr("Audio diagnostics: waiting"), QStringLiteral("audioStatus"));
    fpgaStatus_ = statusLabel(tr("FPGA delivery: waiting"), QStringLiteral("fpgaStatus"));
    statusLayout->addStretch();
    statusScroll->setWidget(statusPanel);
    layout->addWidget(statusScroll, 2);

    log_ = new QPlainTextEdit(central);
    log_->setObjectName(QStringLiteral("statusLog"));
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(200);
    log_->setMinimumHeight(48);
    log_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(log_, 1);
    setCentralWidget(central);
    if (restoreGeometry(settings.value(QStringLiteral("mainWindowGeometry")).toByteArray())) {
        // restoreGeometry relocates off-screen windows. Bound the normal
        // geometry as well when moving between screens of different sizes.
        if (screen() && !isMaximized() && !isFullScreen()) {
            create(); // Obtain the platform frame margins before bounding the restored rectangle.
            const auto available = screen()->availableGeometry();
            const auto decoration = frameSize() - size();
            resize(size().boundedTo(available.size() - decoration));
            const auto maxX = std::max(available.left(), available.right() - frameSize().width() + 1);
            const auto maxY = std::max(available.top(), available.bottom() - frameSize().height() + 1);
            move(std::clamp(x(), available.left(), maxX), std::clamp(y(), available.top(), maxY));
        }
    }

    connect(sourceButton_, &QPushButton::clicked, this, &MainWindow::selectSource);
    connect(startButton_, &QPushButton::clicked, this, &MainWindow::startStreaming);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::stopStreaming);
    connect(diagnosticsCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        QSettings().setValue(QStringLiteral("diagnostics"), enabled);
        updateDiagnosticsVisibility();
    });
    connect(modelineSelect_, &QComboBox::currentIndexChanged, this, &MainWindow::applySelectedModeline);
    connect(crtModeControls_, &CrtModeControls::settingsChanged, this, &MainWindow::applySelectedModeline);
    connect(&streaming_, &StreamingCoordinator::outputModePreview, timingDetails_, &QLabel::setText);
    connect(editModelinesButton_, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(modelinesPath_));
    });
    connect(reloadModelinesButton_, &QPushButton::clicked, this, &MainWindow::reloadModelines);
    connect(horizontalAlignment_, &QComboBox::currentIndexChanged, this, &MainWindow::updateFramingSettings);
    connect(verticalAlignment_, &QComboBox::currentIndexChanged, this, &MainWindow::updateFramingSettings);
    connect(rotation_, &QComboBox::currentIndexChanged, this, &MainWindow::updateFramingSettings);
    connect(scaling_, &QComboBox::currentIndexChanged, this, &MainWindow::updateFramingSettings);
    connect(horizontalOffset_, &QSpinBox::valueChanged, this, &MainWindow::updateFramingSettings);
    connect(verticalOffset_, &QSpinBox::valueChanged, this, &MainWindow::updateFramingSettings);

    connect(&streaming_, &StreamingCoordinator::sourceSelectionStarted, this,
        [this] {
            sourceStatus_->setText(QStringLiteral("Source: opening portal selector..."));
            updateControls();
        });
    connect(&streaming_, &StreamingCoordinator::sourceSelectionCancelled, this, [this] {
        sourceStatus_->setText(QStringLiteral("Source: not selected"));
        appendStatus(QStringLiteral("Desktop source selection cancelled"));
        updateControls();
    });
    connect(&streaming_, &StreamingCoordinator::sourceStarted, this,
        [this](std::uint32_t width, std::uint32_t height, const QString& format) {
            sourceStatus_->setText(QStringLiteral("Source: %1 x %2, %3")
                                       .arg(width).arg(height).arg(format));
            appendStatus(QStringLiteral("Direct PipeWire video capture started"));
            updateControls();
        });
    connect(&streaming_, &StreamingCoordinator::streamConnecting, this,
        [this](bool audioEnabled) {
            streamStatus_->setText(QStringLiteral("Stream: connecting..."));
            latencyStatus_->setText(QStringLiteral("Latency metrics: collecting..."));
            audioStatus_->setText(audioEnabled
                ? QStringLiteral("Audio diagnostics: collecting...")
                : QStringLiteral("Audio: disabled"));
            fpgaStatus_->setText(QStringLiteral("FPGA delivery: collecting..."));
        });
    connect(&streaming_, &StreamingCoordinator::streamStarted, this, [this] {
        streamStatus_->setText(QStringLiteral("Stream: connected"));
        appendStatus(QStringLiteral("Groovy_MiSTer acknowledged the stream"));
        updateControls();
    });
    connect(&streaming_, &StreamingCoordinator::streamStopped, this, [this] {
        streamStatus_->setText(QStringLiteral("Stream: idle"));
        updateControls();
    });
    connect(&streaming_, &StreamingCoordinator::stateChanged,
        this, &MainWindow::updateControls);
    connect(&streaming_, &StreamingCoordinator::failed, this,
        [this](const QString& message) { appendStatus(message, true); });
    connect(&streaming_, &StreamingCoordinator::warning, this,
        [this](const QString& message) { appendStatus(message); });
    connect(&streaming_, &StreamingCoordinator::videoDiagnostics, this,
        [this](const VideoDiagnostics& snapshot) {
            if (diagnosticsCheck_->isChecked()) {
                qInfo().nospace()
                    << "latency frame=" << snapshot.frameNumber
                    << " captured=" << snapshot.capturedFrames
                    << " reused=" << snapshot.reusedFrames
                    << " geometry_rerenders=" << snapshot.rerenderedFrames
                    << " geometry_revision=" << snapshot.processingRevision
                    << " video_processing_failures=" << snapshot.processingFailures
                    << " geometry_us_avg=" << snapshot.averageGeometryUs
                    << " geometry_us_max=" << snapshot.maximumGeometryUs
                    << " geometry_rerender_age_ms=" << snapshot.rerenderAgeMs
                    << " icc_us_avg=" << snapshot.averageIccUs
                    << " icc_us_max=" << snapshot.maximumIccUs
                    << " video_publish_us_avg=" << snapshot.averageVideoPublicationUs
                    << " dequeue_age_ms_p50=" << snapshot.medianDequeueAgeMs
                    << " dequeue_age_ms_p95=" << snapshot.p95DequeueAgeMs
                    << " dequeue_age_ms_max=" << snapshot.maximumDequeueAgeMs
                    << " capture_process_us=" << snapshot.averageCaptureProcessingUs
                    << " ready_wait_us=" << snapshot.averageReadyWaitUs
                    << " lz4_us=" << snapshot.averageCompressionUs
                    << " capacity_wait_us=" << snapshot.averageCapacityWaitUs
                    << " submit_us=" << snapshot.averageSubmissionUs
                    << " payload_bytes=" << snapshot.averagePayloadBytes
                    << " reserve_lines=" << snapshot.deliveryReserveLines
                    << " drops=" << snapshot.intervalDroppedFrames
                    << " full_lz4=" << snapshot.fullLz4Sends
                    << " delta_lz4=" << snapshot.deltaLz4Sends
                    << " delta_content_rejected=" << snapshot.deltaContentRejections
                    << " delta_size_rejected=" << snapshot.deltaSizeRejections
                    << " full_resyncs=" << snapshot.forcedFullResyncs
                    << " history_invalidations=" << snapshot.historyInvalidations;
            }
            latencyStatus_->setText(
                QStringLiteral("Frame %1  |  captured/reused %2/%3  |  new-frame dequeue age p50/p95/max %4/%5/%6 ms  |  capture process/ready %7/%8 us  |  LZ4/queue/submit %9/%10/%11 us  |  %12 B  |  reserve %13 lines  |  interval drops %14")
                    .arg(snapshot.frameNumber)
                    .arg(snapshot.capturedFrames)
                    .arg(snapshot.reusedFrames)
                    .arg(snapshot.medianDequeueAgeMs, 0, 'f', 2)
                    .arg(snapshot.p95DequeueAgeMs, 0, 'f', 2)
                    .arg(snapshot.maximumDequeueAgeMs, 0, 'f', 2)
                    .arg(snapshot.averageCaptureProcessingUs, 0, 'f', 1)
                    .arg(snapshot.averageReadyWaitUs, 0, 'f', 1)
                    .arg(snapshot.averageCompressionUs, 0, 'f', 1)
                    .arg(snapshot.averageCapacityWaitUs, 0, 'f', 1)
                    .arg(snapshot.averageSubmissionUs, 0, 'f', 1)
                    .arg(snapshot.averagePayloadBytes)
                    .arg(snapshot.deliveryReserveLines)
                    .arg(snapshot.intervalDroppedFrames));
        });
    connect(&streaming_, &StreamingCoordinator::audioDiagnostics, this,
        [this](const AudioDiagnostics& snapshot) {
            if (!audioCheck_->isChecked()) {
                audioStatus_->setText(QStringLiteral("Audio: disabled"));
                return;
            }
            if (diagnosticsCheck_->isChecked()) {
                qInfo().nospace()
                    << "audio requested=" << snapshot.requestedPackets
                    << " sent=" << snapshot.sentPackets
                    << " core_disabled=" << snapshot.coreDisabledPackets
                    << " underflow_packets=" << snapshot.underflowPackets
                    << " underflow_frames=" << snapshot.underflowFrames
                    << " stale_frames=" << snapshot.staleFramesDiscarded
                    << " overflow_frames=" << snapshot.overflowFrames
                    << " buffered_frames=" << snapshot.bufferedFrames
                    << " reserve_frames=" << snapshot.jitterReserveFrames
                    << " queue_peak_bytes=" << snapshot.socketQueueHighWaterBytes
                    << " pacing_overruns=" << snapshot.pacingOverruns
                    << " pacing_late_max_ms=" << snapshot.maximumPacingLatenessMs;
            }
            audioStatus_->setText(QStringLiteral(
                "Audio sent/requested %1/%2  |  core disabled %3  |  "
                "underflow packets/frames %4/%5  |  stale/overflow %6/%7 frames  |  "
                "buffered/reserve %8/%9 frames  |  UDP peak %10 KiB  |  pacing overruns %11, max %12 ms")
                .arg(snapshot.sentPackets)
                .arg(snapshot.requestedPackets)
                .arg(snapshot.coreDisabledPackets)
                .arg(snapshot.underflowPackets)
                .arg(snapshot.underflowFrames)
                .arg(snapshot.staleFramesDiscarded)
                .arg(snapshot.overflowFrames)
                .arg(snapshot.bufferedFrames)
                .arg(snapshot.jitterReserveFrames)
                .arg(snapshot.socketQueueHighWaterBytes / 1024.0, 0, 'f', 1)
                .arg(snapshot.pacingOverruns)
                .arg(snapshot.maximumPacingLatenessMs, 0, 'f', 2));
        });
    connect(&streaming_, &StreamingCoordinator::fpgaDiagnostics, this,
        [this](const FpgaDiagnostics& snapshot) {
            if (diagnosticsCheck_->isChecked()) {
                qInfo().nospace()
                    << "fpga samples=" << snapshot.statusSamples
                    << " fallback=" << snapshot.fallbackSamples
                    << " unsynced=" << snapshot.unsyncedSamples
                    << " queue_empty=" << snapshot.queueEmptySamples;
            }
            fpgaStatus_->setText(QStringLiteral(
                "FPGA samples %1  |  fallback %2  |  unsynced %3  |  queue empty %4")
                .arg(snapshot.statusSamples)
                .arg(snapshot.fallbackSamples)
                .arg(snapshot.unsyncedSamples)
                .arg(snapshot.queueEmptySamples));
        });

    reloadModelines();
    updateFramingSettings();
    updateDiagnosticsVisibility();
    updateControls();
}

MainWindow::~MainWindow()
{
    streaming_.shutdown();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    QSettings settings;
    settings.setValue(QStringLiteral("mainWindowGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("misterHost"), hostEdit_->text().trimmed());
    settings.setValue(QStringLiteral("audio"), audioCheck_->isChecked());
    settings.setValue(QStringLiteral("systemGamepads"), gamepadCheck_->isChecked());
    settings.setValue(QStringLiteral("gamepadSlots"), gamepadSlots_->currentData());
    settings.setValue(QStringLiteral("controllerEnabled"), controllerCheck_->isChecked());
    settings.setValue(QStringLiteral("mpvIpcPath"), mpvIpcEdit_->text().trimmed());
    settings.setValue(QStringLiteral("horizontalAlignment"), horizontalAlignment_->currentIndex());
    settings.setValue(QStringLiteral("verticalAlignment"), verticalAlignment_->currentIndex());
    settings.setValue(QStringLiteral("rotation"), rotation_->currentIndex());
    settings.setValue(QStringLiteral("horizontalOffset"), horizontalOffset_->value());
    settings.setValue(QStringLiteral("verticalOffset"), verticalOffset_->value());
    streaming_.shutdown();
    event->accept();
}

void MainWindow::selectSource()
{
    streaming_.selectSource();
}

void MainWindow::startStreaming()
{
    if (!streaming_.sourceReady() || streaming_.streaming()) {
        return;
    }

    streaming_.startStreaming(
        hostEdit_->text().trimmed().toStdString(),
        audioCheck_->isChecked(), controllerCheck_->isChecked(),
        mpvIpcEdit_->text().trimmed().toStdString(), gamepadCheck_->isChecked());

    updateControls();
}

void MainWindow::stopStreaming()
{
    streaming_.stopStreaming();

    updateControls();
}

void MainWindow::appendStatus(const QString& message, bool error)
{
    log_->appendPlainText((error ? QStringLiteral("Error: ") : QString()) + message);
    
    if (error) {
        qWarning().noquote() << message;
    } else {
        qInfo().noquote() << message;
    }
}

void MainWindow::updateControls()
{
    const bool streaming = streaming_.streaming();
    sourceButton_->setEnabled(!streaming);
    startButton_->setEnabled(
        streaming_.sourceReady() && !streaming &&
        !hostEdit_->text().trimmed().isEmpty());
    stopButton_->setEnabled(streaming);
    hostEdit_->setEnabled(!streaming);
    audioCheck_->setEnabled(!streaming);
    controllerCheck_->setEnabled(!streaming);
    gamepadCheck_->setEnabled(!streaming);
    mpvIpcEdit_->setEnabled(!streaming);
}

void MainWindow::updateDiagnosticsVisibility()
{
    const bool visible = diagnosticsCheck_->isChecked();

    latencyStatus_->setVisible(visible);
    audioStatus_->setVisible(visible);
    fpgaStatus_->setVisible(visible);
}

void MainWindow::reloadModelines()
{
    const QString configDirectory =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);

    QDir().mkpath(configDirectory);
    modelinesPath_ = QDir(configDirectory).filePath(QStringLiteral("modelines.dat"));
    
    if (!QFileInfo::exists(modelinesPath_) &&
        !QFile::copy(QStringLiteral(":/mistercast/resources/modelines.dat"), modelinesPath_)) {
        appendStatus(QStringLiteral("Unable to create editable modelines file at %1")
                         .arg(modelinesPath_), true);
    }

    QFile::setPermissions(
        modelinesPath_, QFile::permissions(modelinesPath_) | QFileDevice::WriteUser);

    const auto encodedPath = QFile::encodeName(modelinesPath_);
    auto result = ModelineCatalog::load(
        std::filesystem::path(encodedPath.constData(), encodedPath.constData() + encodedPath.size()));
    
    for (const auto& warning : result.warnings) {
        appendStatus(QStringLiteral("Modeline warning: %1").arg(QString::fromStdString(warning)));
    }

    QSettings settings;
    QString selectedName = settings.value(
        QStringLiteral("modeline"), QString::fromStdString(kDefaultModeline.name)).toString();
    if (modelineSelect_->currentIndex() >= 0 && !modelines_.empty()) {
        selectedName = QString::fromStdString(selectedModeline().name);
    }

    const QSignalBlocker blocker(modelineSelect_);

    modelines_ = std::move(result.modelines);
    modelineSelect_->clear();

    int selectedIndex = 0;

    if (modelines_.empty()) {
        appendStatus(QStringLiteral("No valid modelines found. The manual selector offers the built-in 320x240p60 mode."), true);
        modelineSelect_->addItem(QString::fromStdString(kDefaultModeline.name));
    }
    for (std::size_t i = 0; i < modelines_.size(); ++i) {
        modelineSelect_->addItem(QString::fromStdString(modelines_[i].name));
        if (QString::fromStdString(modelines_[i].name) == selectedName) {
            selectedIndex = static_cast<int>(i);
        }
    }

    modelineSelect_->setCurrentIndex(selectedIndex);
    crtModeControls_->setCatalog(modelines_);

    applySelectedModeline();
    appendStatus(QStringLiteral("Loaded %1 CRT modes from %2")
                     .arg(modelines_.size()).arg(modelinesPath_));
}

void MainWindow::applySelectedModeline()
{
    const auto& mode = selectedModeline();
    modelineSelect_->setEnabled(!crtModeControls_->automatic());
    if (!crtModeControls_->automatic()) {
        QSettings().setValue(QStringLiteral("modeline"), QString::fromStdString(mode.name));
    }
    streaming_.configureOutput(mode, crtModeControls_->request(), modelines_);
}

const Modeline& MainWindow::selectedModeline() const
{
    const int index = modelineSelect_ != nullptr ? modelineSelect_->currentIndex() : -1;

    if (index < 0 || static_cast<std::size_t>(index) >= modelines_.size()) {
        return kDefaultModeline;
    }
    
    return modelines_[static_cast<std::size_t>(index)];
}

void MainWindow::restoreViewport(const Modeline& mode)
{
    QSettings settings;
    viewport_ = loadContentViewport(settings, mode);
    refreshViewport();
}

void MainWindow::saveViewport()
{
    const auto& mode = streaming_.outputMode();
    viewport_ = clampContentViewport(viewport_, mode);
    QSettings settings;
    saveContentViewport(settings, mode, viewport_);
    streaming_.setContentViewport(viewport_);
    refreshViewport();
}

void MainWindow::refreshViewport()
{
    const auto& mode = streaming_.outputMode();
    const auto content = contentRect(viewport_, mode);
    const auto visible = visibleContentRect(content, mode);
    const QSignalBlocker customBlock(customContent_), centerBlock(centerContent_),
        widthBlock(contentWidth_), heightBlock(contentHeight_), xBlock(contentX_), yBlock(contentY_);
    customContent_->setChecked(viewport_.enabled);
    centerContent_->setChecked(viewport_.centered);
    contentWidth_->setRange(1, kMaximumActiveWidth);
    contentHeight_->setRange(1, kMaximumActiveHeight);
    contentX_->setRange(-int(kMaximumActiveWidth), mode.hActive);
    contentY_->setRange(-int(kMaximumActiveHeight), mode.vActive);
    contentWidth_->setValue(content.width);
    contentHeight_->setValue(content.height);
    contentX_->setValue(content.x);
    contentY_->setValue(content.y);
    contentWidth_->setEnabled(viewport_.enabled);
    contentHeight_->setEnabled(viewport_.enabled);
    centerContent_->setEnabled(viewport_.enabled);
    contentX_->setEnabled(viewport_.enabled && !viewport_.centered);
    contentY_->setEnabled(viewport_.enabled && !viewport_.centered);
    rasterSummary_->setText(tr("%1 × %2%3").arg(mode.hActive).arg(mode.vActive)
        .arg(mode.interlaced ? QStringLiteral("i") : QStringLiteral("p")));
    viewportSummary_->setText(tr("%1 × %2 at (%3, %4)").arg(visible.width).arg(visible.height)
        .arg(content.x).arg(content.y));
}

void MainWindow::updateFramingSettings()
{
    FramingSettings settings;
    settings.horizontal = static_cast<HorizontalAlignment>(horizontalAlignment_->currentIndex());
    settings.vertical = static_cast<VerticalAlignment>(verticalAlignment_->currentIndex());
    settings.rotation = static_cast<Rotation>(rotation_->currentIndex());
    settings.offsetX = horizontalOffset_->value();
    settings.offsetY = verticalOffset_->value();
    settings.scaling = selectedScalingAlgorithm(*scaling_);
    QSettings persistent;
    saveScalingSelection(*scaling_, persistent);
    streaming_.setFramingSettings(settings);
}

} // namespace mistercast
