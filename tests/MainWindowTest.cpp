#include "ui/MainWindow.h"
#include "app/CrtGeometryProfile.h"

#include <QCheckBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace mistercast;

class MainWindowTest final : public QObject {
    Q_OBJECT
private slots:
    void resizeAndViewportProfiles();
};

void MainWindowTest::resizeAndViewportProfiles()
{
    QTemporaryDir state;
    QVERIFY(state.isValid());
    qputenv("XDG_CONFIG_HOME", state.path().toUtf8());
    QCoreApplication::setOrganizationName("MiSTerCast-Test");
    QCoreApplication::setApplicationName("MainWindowTest");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, state.path());
    const auto manual = Modeline{"Custom", 14.221170, 664, 692, 759, 873, 498, 501, 507, 543, true};
    {
        const auto ownedWindow = std::make_unique<MainWindow>();
        auto& window = *ownedWindow;
        window.show();
        auto* coordinator = window.findChild<StreamingCoordinator*>();
        auto* settings = window.findChild<QScrollArea*>("settingsScroll");
        auto* statuses = window.findChild<QScrollArea*>("statusScroll");
        auto* diagnostics = window.findChild<QCheckBox*>("showDiagnostics");
        auto* log = window.findChild<QPlainTextEdit*>("statusLog");
        auto* width = window.findChild<QSpinBox*>("contentWidth");
        auto* height = window.findChild<QSpinBox*>("contentHeight");
        auto* x = window.findChild<QSpinBox*>("contentX");
        auto* y = window.findChild<QSpinBox*>("contentY");
        auto* enabled = window.findChild<QCheckBox*>("customContent");
        QVERIFY(coordinator && settings && statuses && diagnostics && log && width && height && x && y && enabled);

        coordinator->configureOutput(manual, std::nullopt, {});
        QCOMPARE(coordinator->outputMode().hActive, 664);
        enabled->setChecked(true);
        width->setValue(640);
        height->setValue(480);
        QCOMPARE(x->value(), 12);
        QCOMPARE(y->value(), 9);
        QCOMPARE(coordinator->outputMode(), manual);
        CrtRequest automatic;
        automatic.profile = *crtProfile("ntsc");
        automatic.width = 320;
        automatic.height = 240;
        automatic.scan = CrtScan::Progressive;
        automatic.cadenceHz = 60;
        coordinator->configureOutput(manual, automatic, {});
        QCOMPARE(coordinator->outputMode().hActive, 320);
        QVERIFY(!enabled->isChecked());
        coordinator->configureOutput(manual, std::nullopt, {});
        QVERIFY(enabled->isChecked());
        QCOMPARE(width->value(), 640);

        QApplication::processEvents();
        const auto originalMinimum = window.minimumSizeHint();
        for (const auto size : {QSize(640,480), QSize(480,360), QSize(600,420), QSize(480,360)}) {
            coordinator->streamConnecting(true);
            coordinator->streamStarted();
            diagnostics->setChecked(true);
            coordinator->videoDiagnostics(VideoDiagnostics{});
            coordinator->audioDiagnostics(AudioDiagnostics{});
            coordinator->fpgaDiagnostics(FpgaDiagnostics{});
            const auto longText = QStringLiteral("Latency: 16.667 ms | Processing: 0.217 ms | ").repeated(24);
            coordinator->outputModePreview(longText);
            for (const auto name : {"sourceStatus", "latencyStatus", "audioStatus", "fpgaStatus"}) {
                auto* label = window.findChild<QLabel*>(name);
                QVERIFY(label && label->wordWrap());
                label->setText(longText);
            }
            log->appendPlainText(longText.repeated(10));
            window.resize(size);
            QApplication::processEvents();
            QCOMPARE(window.size(), size);
            QCOMPARE(window.minimumSizeHint(), originalMinimum);
            QVERIFY(statuses->verticalScrollBar()->maximum() > 0);
            QVERIFY(settings->verticalScrollBar()->maximum() > 0);
            settings->ensureWidgetVisible(height);
            QApplication::processEvents();
            QVERIFY(settings->viewport()->rect().contains(height->mapTo(settings->viewport(), height->rect().center())));
            for (const auto name : {"selectSource", "startStreaming", "stopStreaming"}) {
                auto* button = window.findChild<QPushButton*>(name);
                QVERIFY(button && button->isVisible());
                QVERIFY(window.rect().contains(button->mapTo(&window, button->rect().center())));
            }
            diagnostics->setChecked(false);
            QApplication::processEvents();
            QCOMPARE(window.minimumSizeHint(), originalMinimum);
        }
        window.close();
    }
    const auto restored = std::make_unique<MainWindow>();
    restored->show();
    QApplication::processEvents();
    QCOMPARE(restored->size(), QSize(480,360));
    QSettings stored;
    QCOMPARE(loadContentViewport(stored, manual).width, 640);
    restored->move(10000, 10000);
    stored.setValue("mainWindowGeometry", restored->saveGeometry());
    const auto relocated = std::make_unique<MainWindow>();
    relocated->show();
    QApplication::processEvents();
    QVERIFY(relocated->screen()->availableGeometry().contains(relocated->frameGeometry()));
}

QTEST_MAIN(MainWindowTest)
#include "MainWindowTest.moc"
