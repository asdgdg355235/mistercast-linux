#include "ui/ScalingSelector.h"

#include <QComboBox>
#include <QSettings>

namespace mistercast {
namespace {
QString text(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
}

void populateScalingSelector(QComboBox& selector, const QSettings& settings)
{
    selector.clear();
    for (const auto& descriptor : availableScalers()) {
        selector.addItem(text(descriptor.displayName), text(descriptor.key));
    }
    const auto saved = settings.value(QStringLiteral("scalingAlgorithm")).toString().toStdString();
    const auto algorithm = scalingAlgorithmFromKey(saved).value_or(ScalingAlgorithm::Nearest);
    selector.setCurrentIndex(selector.findData(text(scalerDescriptor(algorithm).key)));
}

ScalingAlgorithm selectedScalingAlgorithm(const QComboBox& selector)
{
    return scalingAlgorithmFromKey(selector.currentData().toString().toStdString())
        .value_or(ScalingAlgorithm::Nearest);
}

void saveScalingSelection(const QComboBox& selector, QSettings& settings)
{
    settings.setValue(QStringLiteral("scalingAlgorithm"),
        text(scalerDescriptor(selectedScalingAlgorithm(selector)).key));
}
}
