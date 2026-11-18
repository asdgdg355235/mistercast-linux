#pragma once

#include "app/StreamingCoordinator.h"
#include "core/Modeline.h"

#include <QMainWindow>

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QSpinBox;

namespace mistercast {

class CrtModeControls;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void selectSource();
    void startStreaming();
    void stopStreaming();
    void appendStatus(const QString& message, bool error = false);
    void updateControls();
    void updateFramingSettings();
    void restoreViewport(const Modeline& mode);
    void refreshViewport();
    void saveViewport();
    void updateDiagnosticsVisibility();
    void reloadModelines();
    void applySelectedModeline();
    [[nodiscard]] const Modeline& selectedModeline() const;

    StreamingCoordinator streaming_;

    ContentViewport viewport_;
    QCheckBox* customContent_{};
    QCheckBox* centerContent_{};
    QSpinBox* contentWidth_{};
    QSpinBox* contentHeight_{};
    QSpinBox* contentX_{};
    QSpinBox* contentY_{};
    QLabel* rasterSummary_{};
    QLabel* viewportSummary_{};

    QLineEdit* hostEdit_{};
    QCheckBox* audioCheck_{};
    QCheckBox* controllerCheck_{};
    QCheckBox* gamepadCheck_{};
    QComboBox* gamepadSlots_{};
    QLineEdit* mpvIpcEdit_{};
    QCheckBox* diagnosticsCheck_{};
    QComboBox* modelineSelect_{};
    CrtModeControls* crtModeControls_{};
    QLabel* timingDetails_{};
    QPushButton* editModelinesButton_{};
    QPushButton* reloadModelinesButton_{};
    QComboBox* horizontalAlignment_{};
    QComboBox* verticalAlignment_{};
    QComboBox* rotation_{};
    QComboBox* scaling_{};
    QSpinBox* horizontalOffset_{};
    QSpinBox* verticalOffset_{};
    QPushButton* sourceButton_{};
    QPushButton* startButton_{};
    QPushButton* stopButton_{};
    QLabel* sourceStatus_{};
    QLabel* streamStatus_{};
    QLabel* latencyStatus_{};
    QLabel* audioStatus_{};
    QLabel* fpgaStatus_{};
    QPlainTextEdit* log_{};
    std::vector<Modeline> modelines_;
    QString modelinesPath_;
};

} // namespace mistercast
