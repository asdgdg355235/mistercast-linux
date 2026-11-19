#pragma once

#include "core/CrtGeometry.h"

#include <QGroupBox>

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace mistercast {

class CrtGeometryControls final : public QGroupBox {
    Q_OBJECT
public:
    explicit CrtGeometryControls(QWidget* parent = nullptr);
    void setMode(const Modeline& mode);

signals:
    void geometryChanged(CrtGeometrySettings geometry);

private:
    CrtGeometrySettings settings() const;
    void changed();
    void updateSummary();

    Modeline mode_;
    bool unsupported_{};
    QCheckBox* enabled_;
    QCheckBox* cabinetEnabled_;
    QSpinBox* hSize_;
    QSpinBox* hPosition_;
    QSpinBox* vShift_;
    QSpinBox* cabinet_;
    QPushButton* reset_;
    QLabel* summary_;
};

} // namespace mistercast
