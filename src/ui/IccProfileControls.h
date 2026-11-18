#pragma once

#include "core/IccTransform.h"

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <cstdint>
#include <functional>

class QCheckBox;
class QFormLayout;
class QLabel;
class QLineEdit;

namespace mistercast {

class IccProfileControls final : public QObject {
public:
    IccProfileControls(QFormLayout& form, std::function<void(IccProcessingSettings)> apply);
    ~IccProfileControls() override;

private:
    void pathChanged();
    void startLoad();
    void refresh();

    std::function<void(IccProcessingSettings)> apply_;
    QCheckBox* enabled_{};
    QCheckBox* calibration_{};
    QLineEdit* path_{};
    QLabel* status_{};
    QThreadPool loader_;
    QTimer debounce_;
    IccProfileLoad profile_;
    std::uint64_t requested_{}, loaded_{};
    bool loading_{};
};

} // namespace mistercast
