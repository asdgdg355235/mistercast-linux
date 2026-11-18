#pragma once

#include "core/CrtGeometry.h"
#include "core/FrameProcessor.h"

class QSettings;

namespace mistercast {

struct CrtGeometryProfile {
    CrtGeometrySettings geometry;
    bool unsupported{};
};

CrtGeometryProfile loadCrtGeometryProfile(QSettings& settings, const Modeline& mode);
void saveCrtGeometryProfile(QSettings& settings, const Modeline& mode, CrtGeometrySettings geometry);

ContentViewport loadContentViewport(QSettings& settings, const Modeline& mode);
void saveContentViewport(QSettings& settings, const Modeline& mode, ContentViewport viewport);

} // namespace mistercast
