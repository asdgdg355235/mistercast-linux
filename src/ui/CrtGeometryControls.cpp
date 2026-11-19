#include "ui/CrtGeometryControls.h"

#include "app/CrtGeometryProfile.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>

namespace mistercast {
namespace {

QSpinBox* amount(QWidget* parent, const char* name, int minimum, int maximum)
{
    auto* spin = new QSpinBox(parent);
    spin->setObjectName(QString::fromLatin1(name));
    spin->setRange(minimum, maximum);
    return spin;
}

} // namespace

CrtGeometryControls::CrtGeometryControls(QWidget* parent)
    : QGroupBox(tr("CRT Geometry"), parent)
    , enabled_(new QCheckBox(tr("Enable geometry"), this))
    , cabinetEnabled_(new QCheckBox(tr("Enable Cabinet Mode"), this))
    , hSize_(amount(this, "hSize", CrtGeometrySettings::minHSize, CrtGeometrySettings::maxHSize))
    , hPosition_(amount(this, "hPosition", CrtGeometrySettings::minHPosition, CrtGeometrySettings::maxHPosition))
    , vShift_(amount(this, "vShift", CrtGeometrySettings::minVShift, CrtGeometrySettings::maxVShift))
    , cabinet_(amount(this, "cabinetVSize", CrtGeometrySettings::minCabinet, CrtGeometrySettings::maxCabinet))
    , reset_(new QPushButton(tr("Reset Geometry"), this))
    , summary_(new QLabel(this))
{
    enabled_->setObjectName(QStringLiteral("geometryEnabled"));
    cabinetEnabled_->setObjectName(QStringLiteral("cabinetEnabled"));
    reset_->setObjectName(QStringLiteral("resetGeometry"));
    hPosition_->setSuffix(tr(" px"));
    hPosition_->setToolTip(tr("Positive moves the picture right by output pixels."));
    vShift_->setToolTip(tr("Positive moves the picture down in whole lines of the current scan mode."));
    cabinet_->setToolTip(tr("Each step adds or removes three lines per field or progressive frame. "
                            "Blends two adjacent lines in linear light using gamma 2.4. "
                            "Picture content beyond the active raster is clipped."));
    summary_->setWordWrap(true);

    auto* layout = new QGridLayout(this);
    layout->addWidget(enabled_, 0, 0, 1, 2);
    layout->addWidget(reset_, 0, 2, 1, 2);
    layout->addWidget(cabinetEnabled_, 1, 0, 1, 4);
    layout->addWidget(new QLabel(tr("H-Size"), this), 2, 0);
    layout->addWidget(hSize_, 2, 1);
    layout->addWidget(new QLabel(tr("H-Position"), this), 2, 2);
    layout->addWidget(hPosition_, 2, 3);
    layout->addWidget(new QLabel(tr("V-Shift"), this), 3, 0);
    layout->addWidget(vShift_, 3, 1);
    layout->addWidget(new QLabel(tr("Cabinet V-Size"), this), 3, 2);
    layout->addWidget(cabinet_, 3, 3);
    layout->addWidget(summary_, 4, 0, 1, 4);

    connect(enabled_, &QCheckBox::toggled, this, &CrtGeometryControls::changed);
    connect(cabinetEnabled_, &QCheckBox::toggled, this, &CrtGeometryControls::changed);
    for (auto* spin : {hSize_, hPosition_, vShift_, cabinet_}) {
        connect(spin, &QSpinBox::valueChanged, this, &CrtGeometryControls::changed);
    }
    connect(reset_, &QPushButton::clicked, this, [this] {
        for (auto* spin : {hSize_, hPosition_, vShift_, cabinet_}) {
            const QSignalBlocker block(spin);
            spin->setValue(0);
        }
        changed();
    });
    setMode(kDefaultModeline);
}

void CrtGeometryControls::setMode(const Modeline& mode)
{
    mode_ = mode;
    QSettings store;
    const auto profile = loadCrtGeometryProfile(store, mode);
    unsupported_ = profile.unsupported;
    const QSignalBlocker enabledBlock(enabled_), hSizeBlock(hSize_), hPositionBlock(hPosition_),
        vShiftBlock(vShift_), cabinetBlock(cabinet_), cabinetEnabledBlock(cabinetEnabled_);
    enabled_->setChecked(profile.geometry.enabled);
    cabinetEnabled_->setChecked(profile.geometry.cabinetEnabled);
    hSize_->setValue(profile.geometry.hSize);
    hPosition_->setValue(profile.geometry.hPosition);
    vShift_->setSuffix(mode.interlaced ? tr(" field-lines") : tr(" rows"));
    vShift_->setValue(profile.geometry.vShift);
    cabinet_->setMinimum(minimumCabinetSize(mode.vActive / (mode.interlaced ? 2 : 1)));
    cabinet_->setValue(profile.geometry.cabinetVSize);
    updateSummary();
}

CrtGeometrySettings CrtGeometryControls::settings() const
{
    return {enabled_->isChecked(), hSize_->value(), hPosition_->value(),
        vShift_->value(), cabinet_->value(), cabinetEnabled_->isChecked()};
}

void CrtGeometryControls::changed()
{
    if (unsupported_ || !validCrtGeometryRaster(mode_)) return;
    updateSummary();
    QSettings store;
    const auto geometry = settings();
    saveCrtGeometryProfile(store, mode_, geometry);
    emit geometryChanged(geometry);
}

void CrtGeometryControls::updateSummary()
{
    const bool valid = validCrtGeometryRaster(mode_);
    enabled_->setEnabled(valid && !unsupported_);
    reset_->setEnabled(valid && !unsupported_);
    cabinetEnabled_->setEnabled(valid && !unsupported_);
    const bool geometryActive = valid && !unsupported_ && enabled_->isChecked();
    for (auto* spin : {hSize_, hPosition_, vShift_}) {
        spin->setEnabled(geometryActive);
    }
    cabinet_->setEnabled(geometryActive && cabinetEnabled_->isChecked());
    if (unsupported_) {
        summary_->setText(tr("This timing has an unsupported geometry profile version."));
        return;
    }
    if (!valid) {
        summary_->setText(tr("Geometry requires a valid active raster and an even interlaced height."));
        return;
    }
    auto requested = settings();
    requested.enabled = true;
    const auto bounds = crtGeometryBounds(mode_, requested);
    const auto units = mode_.interlaced ? tr("field-lines") : tr("rows");
    QString summary = tr("H-Size %1% · Requested height %2 %3")
        .arg((64 + requested.hSize) * 100.0 / 64.0, 0, 'g', 8)
        .arg(bounds.virtualHeight).arg(units);
    if (enabled_->isChecked() && (bounds.left || bounds.right || bounds.top || bounds.bottom)) {
        summary += tr("\nClipped left/right %1/%2 px · top/bottom %3/%4 %5")
            .arg(bounds.left).arg(bounds.right).arg(bounds.top).arg(bounds.bottom).arg(units);
    }
    summary_->setText(summary);
}

} // namespace mistercast
