#include "ui/CrtModeControls.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVariant>
#include <QVBoxLayout>

#include <array>
#include <numeric>

namespace mistercast {
namespace {

QDoubleSpinBox* number(QWidget* parent, double minimum, double maximum, double value, int decimals)
{
    auto* input = new QDoubleSpinBox(parent);
    input->setDecimals(decimals);
    input->setRange(minimum, maximum);
    input->setValue(value);
    input->setKeyboardTracking(false);
    return input;
}

QSpinBox* dimension(QWidget* parent, int maximum, int value)
{
    auto* input = new QSpinBox(parent);
    input->setRange(0, maximum);
    input->setValue(value);
    input->setSpecialValueText(QStringLiteral("Auto"));
    input->setKeyboardTracking(false);
    return input;
}

QHBoxLayout* row(std::initializer_list<QWidget*> widgets)
{
    auto* layout = new QHBoxLayout;
    for (auto* widget : widgets) {
        layout->addWidget(widget);
    }
    return layout;
}

CrtProfile readCustomProfile(const QSettings& settings)
{
    auto profile = *crtProfile("generic_15");
    const auto values = settings.value(QStringLiteral("customProfile")).toList();
    if (values.size() != 14) {
        return profile;
    }

    profile.horizontalHz = {values[0].toDouble(), values[1].toDouble()};
    profile.cadenceHz = {values[2].toDouble(), values[3].toDouble()};
    profile.horizontalUs = {values[4].toDouble(), values[5].toDouble(), values[6].toDouble()};
    profile.verticalMs = {values[7].toDouble(), values[8].toDouble(), values[9].toDouble()};
    profile.progressiveLines = {values[10].toInt(), values[11].toInt()};
    profile.interlacedLines = {values[12].toInt(), values[13].toInt()};
    profile.preferredHorizontalHz = std::midpoint(profile.horizontalHz.minimum, profile.horizontalHz.maximum);
    profile.preferredCadenceHz = std::midpoint(profile.cadenceHz.minimum, profile.cadenceHz.maximum);
    return profile;
}

} // namespace

CrtModeControls::CrtModeControls(QWidget* parent)
    : QWidget(parent)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("crt"));
    customProfile_ = readCustomProfile(settings);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    mode_ = new QComboBox(this);
    mode_->setObjectName(QStringLiteral("crtResolutionMode"));
    mode_->addItems({QStringLiteral("Preloaded / manual modeline"), QStringLiteral("Automatic CRT generation")});
    mode_->setCurrentIndex(settings.value(QStringLiteral("automatic"), false).toBool() ? 1 : 0);
    layout->addWidget(mode_);

    automaticSettings_ = new QWidget(this);
    auto* form = new QFormLayout(automaticSettings_);
    form->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(automaticSettings_);

    width_ = dimension(automaticSettings_, kMaximumActiveWidth, settings.value(QStringLiteral("width"), 0).toInt());
    height_ = dimension(automaticSettings_, kMaximumActiveHeight, settings.value(QStringLiteral("height"), 0).toInt());
    width_->setToolTip(QStringLiteral("Output width in pixels. Auto uses the source or a CRT-sized output."));
    height_->setToolTip(QStringLiteral("Output height in full-frame lines. Auto uses the source or a CRT standard."));
    form->addRow(QStringLiteral("Width × height"), row({width_, height_}));

    cadence_ = number(automaticSettings_, 39.0, 120.0, settings.value(QStringLiteral("refresh"), 0).toDouble(), 6);
    cadence_->setSpecialValueText(QStringLiteral("Profile default"));
    cadence_->setSuffix(QStringLiteral(" Hz"));
    cadence_->setSingleStep(1.0);
    scan_ = new QComboBox(automaticSettings_);
    scan_->addItem(QStringLiteral("Auto scan"), static_cast<int>(CrtScan::Automatic));
    scan_->addItem(QStringLiteral("Progressive"), static_cast<int>(CrtScan::Progressive));
    scan_->addItem(QStringLiteral("Interlaced"), static_cast<int>(CrtScan::Interlaced));
    scan_->setCurrentIndex(qMax(0, scan_->findData(settings.value(QStringLiteral("scan"), 0))));
    form->addRow(QStringLiteral("Frame / field rate"), row({cadence_, scan_}));

    profile_ = new QComboBox(automaticSettings_);
    profile_->addItem(QStringLiteral("NTSC (ntsc)"), QStringLiteral("ntsc"));
    profile_->addItem(QStringLiteral("PAL (pal)"), QStringLiteral("pal"));
    profile_->addItem(QStringLiteral("Generic 15 kHz (generic_15)"), QStringLiteral("generic_15"));
    profile_->addItem(QStringLiteral("Arcade 15 kHz (arcade_15)"), QStringLiteral("arcade_15"));
    profile_->addItem(QStringLiteral("Custom CRT"), QStringLiteral("custom"));
    profile_->setCurrentIndex(qMax(0, profile_->findData(settings.value(QStringLiteral("profile"), QStringLiteral("ntsc")))));
    customButton_ = new QPushButton(QStringLiteral("Edit CRT range…"), automaticSettings_);
    form->addRow(QStringLiteral("Monitor profile"), row({profile_, customButton_}));

    minClock_ = number(automaticSettings_, 2.5, 25.0, settings.value(QStringLiteral("minClock"), 2.5).toDouble(), 3);
    maxClock_ = number(automaticSettings_, 2.5, 25.0, settings.value(QStringLiteral("maxClock"), 25.0).toDouble(), 3);
    tolerance_ = number(automaticSettings_, 0.0, 2.0, settings.value(QStringLiteral("tolerance"), 0.25).toDouble(), 3);
    minClock_->setSuffix(QStringLiteral(" MHz"));
    maxClock_->setSuffix(QStringLiteral(" MHz"));
    tolerance_->setSuffix(QStringLiteral(" Hz"));
    form->addRow(QStringLiteral("Pixel-clock min / max"), row({minClock_, maxClock_}));
    form->addRow(QStringLiteral("Refresh tolerance"), tolerance_);

    fallback_ = new QComboBox(automaticSettings_);
    form->addRow(QStringLiteral("Fallback preset"), fallback_);

    const auto changed = [this] {
        updateVisibility();
        save();
        emit settingsChanged();
    };
    for (auto* input : {mode_, scan_, profile_, fallback_}) {
        connect(input, &QComboBox::currentIndexChanged, this, changed);
    }
    for (auto* input : {width_, height_}) {
        connect(input, &QSpinBox::valueChanged, this, changed);
    }
    for (auto* input : {cadence_, minClock_, maxClock_, tolerance_}) {
        connect(input, &QDoubleSpinBox::valueChanged, this, changed);
    }
    connect(customButton_, &QPushButton::clicked, this, &CrtModeControls::editCustomProfile);
    updateVisibility();
}

bool CrtModeControls::automatic() const
{
    return mode_->currentIndex() == 1;
}

std::optional<CrtRequest> CrtModeControls::request() const
{
    if (!automatic()) {
        return std::nullopt;
    }
    const auto name = profile_->currentData().toString().toStdString();
    return CrtRequest{
        width_->value(), height_->value(),
        cadence_->value() == cadence_->minimum() ? 0.0 : cadence_->value(),
        static_cast<CrtScan>(scan_->currentData().toInt()),
        name == "custom" ? customProfile_ : *crtProfile(name),
        {minClock_->value(), maxClock_->value()},
        tolerance_->value(), fallback_->currentData().toString().toStdString(),
    };
}

void CrtModeControls::setCatalog(std::span<const Modeline> catalog)
{
    const auto saved = QSettings().value(QStringLiteral("crt/fallback")).toString();
    const QSignalBlocker blocker(fallback_);
    fallback_->clear();
    fallback_->addItem(QStringLiteral("Search compatible presets"), QString{});
    for (const auto& mode : catalog) {
        const auto name = QString::fromStdString(mode.name);
        fallback_->addItem(name, name);
    }
    auto index = fallback_->findData(saved);
    if (index < 0) {
        fallback_->addItem(QStringLiteral("%1 (missing)").arg(saved), saved);
        index = fallback_->count() - 1;
    }
    fallback_->setCurrentIndex(index);
}

void CrtModeControls::updateVisibility()
{
    automaticSettings_->setVisible(automatic());
    customButton_->setVisible(profile_->currentData().toString() == QStringLiteral("custom"));
}

void CrtModeControls::save()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("crt"));
    settings.setValue(QStringLiteral("automatic"), automatic());
    settings.setValue(QStringLiteral("width"), width_->value());
    settings.setValue(QStringLiteral("height"), height_->value());
    settings.setValue(QStringLiteral("refresh"), cadence_->value());
    settings.setValue(QStringLiteral("scan"), scan_->currentData());
    settings.setValue(QStringLiteral("profile"), profile_->currentData());
    settings.setValue(QStringLiteral("fallback"), fallback_->currentData());
    settings.setValue(QStringLiteral("minClock"), minClock_->value());
    settings.setValue(QStringLiteral("maxClock"), maxClock_->value());
    settings.setValue(QStringLiteral("tolerance"), tolerance_->value());
}

void CrtModeControls::editCustomProfile()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Custom CRT operating range"));
    auto* layout = new QFormLayout(&dialog);
    const auto& p = customProfile_;
    const auto interval = [&](const QString& label, CrtInterval values,
                              double minimum, double maximum, int decimals) {
        auto* first = number(&dialog, minimum, maximum, values.minimum, decimals);
        auto* last = number(&dialog, minimum, maximum, values.maximum, decimals);
        layout->addRow(label, row({first, last}));
        return std::array{first, last};
    };
    const auto porches = [&](const QString& label, CrtPorches values, double maximum) {
        auto* front = number(&dialog, 0.001, maximum, values.front, 3);
        auto* sync = number(&dialog, 0.001, maximum, values.sync, 3);
        auto* back = number(&dialog, 0.001, maximum, values.back, 3);
        layout->addRow(label, row({front, sync, back}));
        return std::array{front, sync, back};
    };
    const auto lines = [&](const QString& label, CrtLineRange values) {
        auto* first = dimension(&dialog, kMaximumActiveHeight, values.minimum);
        auto* last = dimension(&dialog, kMaximumActiveHeight, values.maximum);
        first->setSpecialValueText(QStringLiteral("Disabled"));
        last->setSpecialValueText(QStringLiteral("Disabled"));
        layout->addRow(label, row({first, last}));
        return std::array{first, last};
    };

    const auto horizontal = interval(QStringLiteral("Horizontal min / max (Hz)"), p.horizontalHz, 14000, 540672, 2);
    const auto cadence = interval(QStringLiteral("Frame / field min / max (Hz)"), p.cadenceHz, 40, 120, 6);
    const auto hPorch = porches(QStringLiteral("H front / sync / back (µs)"), p.horizontalUs, 70);
    const auto vPorch = porches(QStringLiteral("V front / sync / back (ms)"), p.verticalMs, 25);
    const auto progressive = lines(QStringLiteral("Progressive active lines min / max"), p.progressiveLines);
    const auto interlaced = lines(QStringLiteral("Interlaced active lines min / max"), p.interlacedLines);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QSettings settings;
    settings.beginGroup(QStringLiteral("crt"));
    settings.setValue(QStringLiteral("customProfile"), QVariantList{
        horizontal[0]->value(), horizontal[1]->value(), cadence[0]->value(), cadence[1]->value(),
        hPorch[0]->value(), hPorch[1]->value(), hPorch[2]->value(),
        vPorch[0]->value(), vPorch[1]->value(), vPorch[2]->value(),
        progressive[0]->value(), progressive[1]->value(), interlaced[0]->value(), interlaced[1]->value(),
    });
    customProfile_ = readCustomProfile(settings);
    emit settingsChanged();
}

} // namespace mistercast
