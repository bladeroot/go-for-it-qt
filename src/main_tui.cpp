#include "core/AppController.h"
#include "tui/TuiApp.h"
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QLoggingCategory>
#include <QTextStream>

#ifndef GOFI_VERSION
#define GOFI_VERSION "2.0.0"
#endif

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsEmpty("QT_FFMPEG_DECODING_HW_DEVICE_TYPES")) {
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",");
    }
    QLoggingCategory::setFilterRules(QStringLiteral(
        "qt.multimedia.ffmpeg.info=false\n"
        "qt.multimedia.ffmpeg.debug=false\n"
        "qt.qpa.services.warning=false"
    ));

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("go-for-it-qt"));
    QCoreApplication::setApplicationVersion(QStringLiteral(GOFI_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("go-for-it-qt"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Go for It Qt — Terminal UI (TUI) with Vim keybindings"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption listOption(QStringLiteral("list"), QStringLiteral("Show configured lists and exit"));
    QCommandLineOption logFileOption(QStringLiteral("logfile"), QStringLiteral("CSV file to log activities to."), QStringLiteral("FILE"));
    QCommandLineOption loadOption(QStringLiteral("load"), QStringLiteral("Load the list specified by ID."), QStringLiteral("LIST-ID"));
    QCommandLineOption dbOption(QStringLiteral("db"), QStringLiteral("Custom SQLite database file path."), QStringLiteral("DB-FILE"));
    QCommandLineOption mdOption(QStringLiteral("md"), QStringLiteral("Open a Markdown (.md) file directly in the Vim Markdown viewer."), QStringLiteral("MD-FILE"));
    parser.addOption(listOption);
    parser.addOption(logFileOption);
    parser.addOption(loadOption);
    parser.addOption(dbOption);
    parser.addOption(mdOption);
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
            out << '"' << gofi::TuiApp::sanitizeTerminalText(e.provider)
                << "\" : \"" << gofi::TuiApp::sanitizeTerminalText(e.id)
                << "\" - \"" << gofi::TuiApp::sanitizeTerminalText(e.name) << "\"\n";
        }
        return 0;
    }

    if (parser.isSet(loadOption)) {
        controller.selectListById(parser.value(loadOption));
    }

    gofi::TuiApp tui(&controller);
    if (!tui.start()) {
        QTextStream err(stderr);
        err << "Error: go-for-it-tui requires an interactive terminal (TTY).\n";
        return 1;
    }
    if (parser.isSet(mdOption)) {
        tui.openMarkdownFile(parser.value(mdOption));
    }
    return app.exec();
}
