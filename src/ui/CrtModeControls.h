#pragma once

#include "core/CrtModeline.h"

#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QSpinBox;

namespace mistercast {

class CrtModeControls final : public QWidget {
    Q_OBJECT

public:
    explicit CrtModeControls(QWidget* parent = nullptr);

    [[nodiscard]] bool automatic() const;
    [[nodiscard]] std::optional<CrtRequest> request() const;
    void setCatalog(std::span<const Modeline> catalog);

signals:
    void settingsChanged();

private:
    void editCustomProfile();
    void save();
    void updateVisibility();

    QComboBox* mode_{};
    QWidget* automaticSettings_{};
    QSpinBox* width_{};
    QSpinBox* height_{};
    QDoubleSpinBox* cadence_{};
    QComboBox* scan_{};
    QComboBox* profile_{};
    QPushButton* customButton_{};
    QComboBox* fallback_{};
    QDoubleSpinBox* minClock_{};
    QDoubleSpinBox* maxClock_{};
    QDoubleSpinBox* tolerance_{};
    CrtProfile customProfile_;
};

} // namespace mistercast
