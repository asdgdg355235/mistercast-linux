#include "ui/IccProfileControls.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSettings>
#include <QWidget>

#include <exception>
#include <utility>

namespace mistercast {

IccProfileControls::IccProfileControls(QFormLayout& form,
                                       std::function<void(IccProcessingSettings)> apply)
    : QObject(form.parentWidget()), apply_(std::move(apply))
{
    auto* parent = form.parentWidget();
    QSettings settings;
    enabled_ = new QCheckBox(tr("Enable ICC color correction"), parent);
    enabled_->setChecked(settings.value(QStringLiteral("icc/enabled"), false).toBool());
    enabled_->setToolTip(tr("Converts assumed sRGB capture pixels to the selected display profile."));
    form.addRow(QString(), enabled_);

    auto* row = new QHBoxLayout;
    path_ = new QLineEdit(settings.value(QStringLiteral("icc/path")).toString(), parent);
    path_->setPlaceholderText(tr("Absolute path to a display profile"));
    auto* browse = new QPushButton(tr("Browse…"), parent);
    row->addWidget(path_, 1);
    row->addWidget(browse);
    form.addRow(tr("ICC profile"), row);

    calibration_ = new QCheckBox(tr("Apply embedded calibration (vcgt)"), parent);
    calibration_->setChecked(settings.value(QStringLiteral("icc/calibration"), false).toBool());
    calibration_->setToolTip(tr("Applies the profile's video-card gamma table after color conversion. "
                                "Use only when the calibration is not already active on the MiSTer output."));
    form.addRow(QString(), calibration_);

    status_ = new QLabel(parent);
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    form.addRow(QString(), status_);

    loader_.setMaxThreadCount(1);
    debounce_.setSingleShot(true);
    debounce_.setInterval(260);
    connect(&debounce_, &QTimer::timeout, this, &IccProfileControls::startLoad);
    connect(path_, &QLineEdit::textChanged, this, &IccProfileControls::pathChanged);
    connect(path_, &QLineEdit::editingFinished, this, [this] {
        debounce_.stop();
        startLoad();
    });
    connect(browse, &QPushButton::clicked, this, [this, parent] {
        const auto path = QFileDialog::getOpenFileName(parent, tr("Select ICC display profile"),
            path_->text(), tr("ICC profiles (*.icc *.icm *.ICC *.ICM)"));
        if (!path.isEmpty()) {
            path_->setText(path);
            debounce_.stop();
            startLoad();
        }
    });
    connect(enabled_, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue(QStringLiteral("icc/enabled"), checked);
        refresh();
    });
    connect(calibration_, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue(QStringLiteral("icc/calibration"), checked);
        refresh();
    });

    ++requested_;
    startLoad();
}

IccProfileControls::~IccProfileControls()
{
    // The loader only posts back to this object. Join before QObject removes
    // the queued completion events. Capture snapshots own independent refs.
    loader_.waitForDone();
}

void IccProfileControls::pathChanged()
{
    QSettings().setValue(QStringLiteral("icc/path"), path_->text().trimmed());
    ++requested_;
    profile_ = {};
    debounce_.start();
    refresh();
}

void IccProfileControls::startLoad()
{
    if (loading_ || loaded_ == requested_) return;
    const auto path = path_->text().trimmed().toStdString();
    const auto revision = requested_;
    if (path.empty()) {
        loaded_ = revision;
        refresh();
        return;
    }

    loading_ = true;
    refresh();
    loader_.start([this, revision, path] {
        IccProfileLoad result;
        try {
            result = IccTransform::load(path);
        } catch (const std::exception&) {
            result.error = "Unable to prepare the ICC profile.";
        }
        QMetaObject::invokeMethod(this, [this, revision, result = std::move(result)] {
            loading_ = false;
            if (revision == requested_) {
                loaded_ = revision;
                profile_ = std::move(result);
                refresh();
            } else if (!debounce_.isActive()) {
                startLoad();
            }
        }, Qt::QueuedConnection);
    });
}

void IccProfileControls::refresh()
{
    const bool hasCalibration = profile_.transform && profile_.transform->hasCalibration();
    const bool active = enabled_->isChecked() && bool(profile_.transform);
    const bool calibrate = active && hasCalibration && calibration_->isChecked();
    calibration_->setEnabled(active && hasCalibration);
    apply_({active ? profile_.transform : nullptr, calibrate});

    QString message;
    if (profile_.transform) {
        message = active ? tr("ICC profile active.") : tr("ICC profile loaded. Correction is off.");
        if (hasCalibration) {
            message += calibrate ? tr(" Embedded calibration is on.") : tr(" Embedded calibration is off.");
        } else if (profile_.transform->calibrationBakedIn()) {
            message += tr(" Argyll calibration is already built into the profile. Separate calibration is off.");
        } else if (profile_.transform->malformedCalibration()) {
            message += tr(" Embedded calibration could not be decoded.");
        } else {
            message += tr(" No embedded calibration.");
        }
    } else if (path_->text().trimmed().isEmpty()) {
        if (enabled_->isChecked()) message = tr("Select an ICC profile. Correction is off.");
    } else if (loaded_ != requested_) {
        message = tr("Loading ICC profile. Correction is off.");
    } else {
        message = QString::fromStdString(profile_.error) + tr(" Correction is off.");
    }
    status_->setText(message);
    status_->setVisible(!message.isEmpty());
}

} // namespace mistercast
