#include "ui/CrtGeometryControls.h"
#include "app/CrtGeometryProfile.h"

#include <QCheckBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

using namespace mistercast;

class CrtGeometryControlsTest : public QObject {
    Q_OBJECT
private slots:
    void timingProfiles();
};

void CrtGeometryControlsTest::timingProfiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QCoreApplication::setOrganizationName("mistercast-geometry-test");
    QCoreApplication::setApplicationName("profiles");

    CrtGeometryControls panel;
    QSettings store;
    auto* on = panel.findChild<QCheckBox*>("geometryEnabled");
    auto* cabinet = panel.findChild<QCheckBox*>("cabinetEnabled");
    auto* h = panel.findChild<QSpinBox*>("hSize");
    auto* x = panel.findChild<QSpinBox*>("hPosition");
    auto* y = panel.findChild<QSpinBox*>("vShift");
    auto* v = panel.findChild<QSpinBox*>("cabinetVSize");
    auto* reset = panel.findChild<QPushButton*>("resetGeometry");
    QVERIFY(on && cabinet && h && x && y && v && reset);
    QVERIFY(!on->isChecked() && !cabinet->isChecked() && !h->isEnabled() && !v->isEnabled());
    CrtGeometrySettings observed;
    unsigned emitted = 0;
    connect(&panel, &CrtGeometryControls::geometryChanged, this, [&](CrtGeometrySettings settings) {
        ++emitted;
        observed = settings;
    });
    on->setChecked(true);
    QVERIFY(h->isEnabled() && x->isEnabled() && y->isEnabled() && !v->isEnabled());
    cabinet->setChecked(true);
    QVERIFY(v->isEnabled());
    h->setValue(3);
    x->setValue(-7);
    y->setValue(4);
    v->setValue(6);
    const CrtGeometrySettings ntsc{true, 3, -7, 4, 6, true};
    QVERIFY(observed == ntsc);
    QVERIFY(loadCrtGeometryProfile(store, kDefaultModeline).geometry == ntsc);
    const auto key = store.allKeys().filter(QRegularExpression(QStringLiteral("/cabinetEnabled$"))).value(0);
    QVERIFY(!key.isEmpty());
    store.remove(key); // Same version and timing key as a profile saved before the toggle.
    QVERIFY(loadCrtGeometryProfile(store, kDefaultModeline).geometry == ntsc);
    const auto savedGeometryKey = key.chopped(QStringLiteral("cabinetEnabled").size()) + QStringLiteral("enabled");
    store.setValue(savedGeometryKey, false);
    const auto legacyDisabled = loadCrtGeometryProfile(store, kDefaultModeline).geometry;
    QVERIFY(!legacyDisabled.enabled && !legacyDisabled.cabinetEnabled);
    QCOMPARE(legacyDisabled.cabinetVSize, 6);
    store.setValue(savedGeometryKey, true);
    const auto sizeKey = key.chopped(QStringLiteral("cabinetEnabled").size()) + QStringLiteral("cabinetVSize");
    store.setValue(sizeKey, 0);
    QVERIFY(!loadCrtGeometryProfile(store, kDefaultModeline).geometry.cabinetEnabled);
    store.setValue(sizeKey, 6);

    cabinet->setChecked(false);
    QVERIFY(!v->isEnabled() && h->isEnabled());
    QCOMPARE(v->value(), 6);
    QVERIFY(!loadCrtGeometryProfile(store, kDefaultModeline).geometry.cabinetEnabled);
    cabinet->setChecked(true);
    QVERIFY(observed == ntsc);


    auto pal = kDefaultModeline;
    pal.vActive = 576;
    pal.vTotal = 625;
    pal.interlaced = true;
    panel.setMode(pal);
    QVERIFY(!on->isChecked());
    QCOMPARE(h->value(), 0);
    QVERIFY(y->suffix().contains("field-lines"));
    on->setChecked(true);
    h->setValue(-10);
    cabinet->setChecked(true);
    v->setValue(2);
    cabinet->setChecked(false);
    auto renamed = kDefaultModeline;
    renamed.name = "Same timing";
    panel.setMode(renamed);
    QCOMPARE(h->value(), 3);
    QCOMPARE(x->value(), -7);
    QCOMPARE(v->value(), 6);
    QVERIFY(cabinet->isChecked() && v->isEnabled());
    const auto before = emitted;
    reset->click();
    QCOMPARE(emitted, before + 1);
    QVERIFY(on->isChecked() && observed.enabled && observed.cabinetEnabled && !observed.effective());
    panel.setMode(pal);
    QVERIFY(on->isChecked());
    QCOMPARE(h->value(), -10);
    QVERIFY(!cabinet->isChecked() && !v->isEnabled());
    QCOMPARE(v->value(), 2);
    on->setChecked(false);
    cabinet->setChecked(true);
    QVERIFY(cabinet->isChecked() && !v->isEnabled() && !h->isEnabled());
    QCOMPARE(h->value(), -10);

    const auto keys = store.allKeys();
    for (const auto& key : keys) {
        if (key.endsWith("algorithmVersion")) store.setValue(key, 99);
    }
    panel.setMode(pal);
    QVERIFY(!on->isEnabled());
    QVERIFY(!loadCrtGeometryProfile(store, pal).geometry.enabled);
}

QTEST_MAIN(CrtGeometryControlsTest)
#include "CrtGeometryControlsTest.moc"
