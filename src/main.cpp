#include "core/AppController.h"
#include "tui/TuiApp.h"
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTextStream>

#ifndef GOFI_VERSION
#define GOFI_VERSION "2.0.1"
#endif

int main(int argc, char* argv[]) {
    // Audio-only usage of QtMultimedia: avoid probing GPU video decoding backends (VDPAU/VAAPI)
    if (qEnvironmentVariableIsEmpty("QT_FFMPEG_DECODING_HW_DEVICE_TYPES")) {
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",");
    }
    QLoggingCategory::setFilterRules(QStringLiteral(
        "qt.multimedia.ffmpeg.info=false\n"
        "qt.multimedia.ffmpeg.debug=false\n"
        "qt.qpa.services.warning=false"
    ));

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("go-for-it-qt"));
    QApplication::setApplicationDisplayName(QStringLiteral("Go for It Qt"));
    QApplication::setApplicationVersion(QStringLiteral(GOFI_VERSION));
    QApplication::setOrganizationName(QStringLiteral("go-for-it-qt"));
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, QStringLiteral("go-for-it-qt.desktop")).isEmpty()) {
        QApplication::setDesktopFileName(QStringLiteral("go-for-it-qt"));
    }

    // Use native/system Fusion desktop style unless overridden via environment
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        QQuickStyle::setStyle(QStringLiteral("Fusion"));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A stylish to-do list with built-in productivity timer"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption aboutOption({QStringLiteral("a"), QStringLiteral("about")}, QStringLiteral("Show about dialog"));
    QCommandLineOption listOption(QStringLiteral("list"), QStringLiteral("Show configured lists and exit"));
    QCommandLineOption logFileOption(QStringLiteral("logfile"), QStringLiteral("CSV file to log activities to."), QStringLiteral("FILE"));
    QCommandLineOption loadOption(QStringLiteral("load"), QStringLiteral("Load the list specified by ID."), QStringLiteral("LIST-ID"));
    QCommandLineOption dbOption(QStringLiteral("db"), QStringLiteral("Custom SQLite database file path."), QStringLiteral("DB-FILE"));
    QCommandLineOption tuiOption({QStringLiteral("t"), QStringLiteral("tui")}, QStringLiteral("Run in console Terminal UI (TUI) mode with Vim keybindings."));
    parser.addOption(aboutOption);
    parser.addOption(listOption);
    parser.addOption(logFileOption);
    parser.addOption(loadOption);
    parser.addOption(dbOption);
    parser.addOption(tuiOption);
    parser.process(app);

    const QString customDbPath = parser.value(dbOption);
    if (parser.isSet(dbOption)) {
        if (customDbPath.isEmpty() || !gofi::DatabaseManager::instance().initialize(customDbPath)) {
            QTextStream err(stderr);
            err << "Error: Failed to open database at: " << customDbPath << "\n";
            return 1;
        }
    }

    gofi::AppController controller(parser.value(logFileOption), customDbPath);

    if (parser.isSet(listOption)) {
        QTextStream out(stdout);
        out << "Lists (List type : List ID - List name):\n";
        for (const gofi::TodoListEntry& e : controller.listManager()->entries()) {
            out << '"' << e.provider << "\" : \"" << e.id << "\" - \"" << e.name << "\"\n";
        }
        return 0;
    }

    if (parser.isSet(loadOption)) {
        controller.selectListById(parser.value(loadOption));
    }

    if (parser.isSet(tuiOption)) {
        gofi::TuiApp tui(&controller);
        if (!tui.start()) {
            QTextStream err(stderr);
            err << "Error: --tui requires an interactive terminal (TTY).\n";
            return 1;
        }
        return app.exec();
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("showAboutOnStart"), parser.isSet(aboutOption));

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection
    );

    QObject::connect(&controller, &gofi::AppController::languageChanged, &engine, &QQmlApplicationEngine::retranslate);
    engine.loadFromModule("GoForIt", "Main");
    return app.exec();
}
