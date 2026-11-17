#pragma once

#include "core/CrtGeometry.h"

class QSettings;

namespace mistercast {

struct CrtGeometryProfile {
    CrtGeometrySettings geometry;
    bool unsupported{};
};

CrtGeometryProfile loadCrtGeometryProfile(QSettings& settings, const Modeline& mode);
void saveCrtGeometryProfile(QSettings& settings, const Modeline& mode, CrtGeometrySettings geometry);

} // namespace mistercast
