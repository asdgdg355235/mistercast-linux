#pragma once

#include "core/Scaler.h"

class QComboBox;
class QSettings;

namespace mistercast {
void populateScalingSelector(QComboBox& selector, const QSettings& settings);
ScalingAlgorithm selectedScalingAlgorithm(const QComboBox& selector);
void saveScalingSelection(const QComboBox& selector, QSettings& settings);
}
