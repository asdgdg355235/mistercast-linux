#include "ui/CrtGeometryControls.h"
#include "app/CrtGeometryProfile.h"

#include <QCheckBox>
#include <QPushButton>
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
    auto* h = panel.findChild<QSpinBox*>("hSize");
    auto* x = panel.findChild<QSpinBox*>("hPosition");
    auto* y = panel.findChild<QSpinBox*>("vShift");
    auto* v = panel.findChild<QSpinBox*>("cabinetVSize");
    auto* reset = panel.findChild<QPushButton*>("resetGeometry");
    QVERIFY(on && h && x && y && v && reset);
    QVERIFY(!on->isChecked() && !h->isEnabled());
    CrtGeometrySettings observed;
    unsigned emitted = 0;
    connect(&panel, &CrtGeometryControls::geometryChanged, this, [&](CrtGeometrySettings settings) {
        ++emitted;
        observed = settings;
    });
    on->setChecked(true);
    h->setValue(3);
    x->setValue(-7);
    y->setValue(4);
    v->setValue(6);
    const CrtGeometrySettings ntsc{true, 3, -7, 4, 6};
    QVERIFY(observed == ntsc);
    QVERIFY(loadCrtGeometryProfile(store, kDefaultModeline).geometry == ntsc);

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
    auto renamed = kDefaultModeline;
    renamed.name = "Same timing";
    panel.setMode(renamed);
    QCOMPARE(h->value(), 3);
    QCOMPARE(x->value(), -7);
    QCOMPARE(v->value(), 6);
    const auto before = emitted;
    reset->click();
    QCOMPARE(emitted, before + 1);
    QVERIFY(on->isChecked() && observed.enabled && !observed.effective());
    panel.setMode(pal);
    QVERIFY(on->isChecked());
    QCOMPARE(h->value(), -10);
    on->setChecked(false);
    QVERIFY(!h->isEnabled());
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
