#include "app/CrtGeometryProfile.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QSettings>

namespace mistercast {
namespace {

// Encode the exact double sent in the mode packet. The storage byte order is
// fixed so the key survives changes of architecture, locale and display name.
QByteArray timingTuple(const Modeline& mode)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODeviceBase::WriteOnly);
    out.setFloatingPointPrecision(QDataStream::DoublePrecision);
    out << mode.pixelClockMHz
        << quint16(mode.hActive) << quint16(mode.hBegin) << quint16(mode.hEnd) << quint16(mode.hTotal)
        << quint16(mode.vActive) << quint16(mode.vBegin) << quint16(mode.vEnd) << quint16(mode.vTotal)
        << quint8(mode.interlaced);
    return bytes;
}

QString profilePath(const QByteArray& tuple, QString prefix = QStringLiteral("crtGeometry/v1/profiles/"))
{
    return prefix +
        QString::fromLatin1(QCryptographicHash::hash(tuple, QCryptographicHash::Sha256).toHex()) +
        QLatin1Char('/');
}

} // namespace

CrtGeometryProfile loadCrtGeometryProfile(QSettings& settings, const Modeline& mode)
{
    const auto tuple = timingTuple(mode);
    const auto path = profilePath(tuple);
    if (!settings.contains(path + QStringLiteral("algorithmVersion"))) return {};
    if (settings.value(path + QStringLiteral("algorithmVersion")).toInt() !=
            CrtGeometrySettings::algorithmVersion ||
        settings.value(path + QStringLiteral("timing")).toByteArray() != tuple) return {{}, true};

    const bool enabled = settings.value(path + QStringLiteral("enabled")).toBool();
    const int cabinet = settings.value(path + QStringLiteral("cabinetVSize")).toInt();
    // Version 1 profiles without the new key used Cabinet whenever its
    // amount was nonzero and the geometry master switch was on.
    const bool cabinetEnabled = settings.value(path + QStringLiteral("cabinetEnabled"),
                                               enabled && cabinet != 0).toBool();
    return {clampCrtGeometry({
        enabled,
        settings.value(path + QStringLiteral("hSize")).toInt(),
        settings.value(path + QStringLiteral("hPosition")).toInt(),
        settings.value(path + QStringLiteral("vShift")).toInt(),
        cabinet, cabinetEnabled
    }, mode), false};
}

void saveCrtGeometryProfile(QSettings& settings, const Modeline& mode, CrtGeometrySettings geometry)
{
    const auto tuple = timingTuple(mode);
    const auto path = profilePath(tuple);
    if (loadCrtGeometryProfile(settings, mode).unsupported) return;
    geometry = clampCrtGeometry(geometry, mode);
    settings.setValue(path + QStringLiteral("algorithmVersion"), CrtGeometrySettings::algorithmVersion);
    settings.setValue(path + QStringLiteral("timing"), tuple);
    settings.setValue(path + QStringLiteral("enabled"), geometry.enabled);
    settings.setValue(path + QStringLiteral("hSize"), geometry.hSize);
    settings.setValue(path + QStringLiteral("hPosition"), geometry.hPosition);
    settings.setValue(path + QStringLiteral("vShift"), geometry.vShift);
    settings.setValue(path + QStringLiteral("cabinetVSize"), geometry.cabinetVSize);
    settings.setValue(path + QStringLiteral("cabinetEnabled"), geometry.cabinetEnabled);
}

ContentViewport loadContentViewport(QSettings& settings, const Modeline& mode)
{
    const auto path = profilePath(timingTuple(mode), QStringLiteral("contentViewport/v1/profiles/"));
    return clampContentViewport({settings.value(path + QStringLiteral("enabled"), false).toBool(),
        settings.value(path + QStringLiteral("centered"), true).toBool(),
        settings.value(path + QStringLiteral("width")).toInt(),
        settings.value(path + QStringLiteral("height")).toInt(),
        settings.value(path + QStringLiteral("x")).toInt(),
        settings.value(path + QStringLiteral("y")).toInt()}, mode);
}

void saveContentViewport(QSettings& settings, const Modeline& mode, ContentViewport viewport)
{
    const auto path = profilePath(timingTuple(mode), QStringLiteral("contentViewport/v1/profiles/"));
    viewport = clampContentViewport(viewport, mode);
    settings.setValue(path + QStringLiteral("enabled"), viewport.enabled);
    settings.setValue(path + QStringLiteral("centered"), viewport.centered);
    settings.setValue(path + QStringLiteral("width"), viewport.width);
    settings.setValue(path + QStringLiteral("height"), viewport.height);
    settings.setValue(path + QStringLiteral("x"), viewport.x);
    settings.setValue(path + QStringLiteral("y"), viewport.y);
}

} // namespace mistercast
