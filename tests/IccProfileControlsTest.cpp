#include "ui/IccProfileControls.h"
#include "IccFixtures.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace mistercast {

class IccProfileControlsTest : public QObject {
    Q_OBJECT

private slots:
    void liveChangesAndPersistence();
};

void IccProfileControlsTest::liveChangesAndPersistence()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("mistercast_test"));
    QCoreApplication::setApplicationName(QStringLiteral("icc_profile_controls"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QSettings().clear();

    const auto matrix = dir.filePath(QStringLiteral("matrix.icm"));
    const auto lut = dir.filePath(QStringLiteral("CRT 測定 #1.icc"));
    QVERIFY(icc_test::writeDisplay(matrix.toStdString(), false, false));
    QVERIFY(icc_test::writeDisplay(lut.toStdString(), true, true));

    IccProcessingSettings applied;
    const auto apply = [&](IccProcessingSettings settings) { applied = std::move(settings); };
    {
        QWidget container;
        QFormLayout form(&container);
        new IccProfileControls(form, apply);
        const auto checkboxes = container.findChildren<QCheckBox*>();
        QCOMPARE(checkboxes.size(), 2);
        auto* enabled = checkboxes[0];
        auto* calibration = checkboxes[1];
        auto* path = container.findChild<QLineEdit*>();
        auto* status = qobject_cast<QLabel*>(form.itemAt(3, QFormLayout::FieldRole)->widget());
        QVERIFY(path && status);
        QVERIFY(!enabled->isChecked() && !calibration->isChecked());

        enabled->setChecked(true);
        path->setText(lut);
        QTRY_VERIFY(applied.transform && applied.transform->hasCalibration());
        QVERIFY(calibration->isEnabled() && !applied.applyCalibration);
        const auto active = applied.transform;
        calibration->setChecked(true);
        QCOMPARE(applied.transform, active);
        QVERIFY(applied.applyCalibration);
        enabled->setChecked(false);
        QVERIFY(!applied.transform);
        enabled->setChecked(true);
        QCOMPARE(applied.transform, active);

        path->setText(dir.filePath(QStringLiteral("missing.icc")));
        QTRY_VERIFY(status->text().contains(QStringLiteral("accessible file")));
        QVERIFY(!applied.transform);
        path->setText(matrix);
        QTRY_VERIFY(applied.transform && !applied.transform->hasCalibration());
        QVERIFY(!calibration->isEnabled() && !applied.applyCalibration);

        // A profile that finishes loading after a newer request cannot
        // replace it. Editing-finished starts the worker without debounce.
        path->setText(lut);
        QVERIFY(QMetaObject::invokeMethod(path, "editingFinished"));
        path->setText(matrix);
        QVERIFY(!applied.transform);
        QTRY_VERIFY(applied.transform && !applied.transform->hasCalibration());
        path->setText(lut);
        QVERIFY(QMetaObject::invokeMethod(path, "editingFinished"));
        // Destroy the widget while a load result may still be queued.
    }
    applied = {};
    {
        QWidget container;
        QFormLayout form(&container);
        new IccProfileControls(form, apply);
        QCOMPARE(container.findChild<QLineEdit*>()->text(), lut);
        QTRY_VERIFY(applied.transform && applied.transform->hasCalibration());
        QVERIFY(applied.applyCalibration);
    }
}

} // namespace mistercast

QTEST_MAIN(mistercast::IccProfileControlsTest)
#include "IccProfileControlsTest.moc"
