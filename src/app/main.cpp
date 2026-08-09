#include "app/SessionLog.h"
#include "host/PluginManager.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "ui/AppController.h"

#include "core/CoreMetaTypes.h"
#include "core/diagnostics/Diagnostics.h"

#include <QDir>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTextStream>

namespace {

/// Prints what the binary is and what it found, then exits. Handy when a
/// packaged build misbehaves: it answers "did the plugin load?" without
/// needing a display.
int runDiagnostics(mole::AppController& controller, bool listPlugins)
{
    QTextStream out(stdout);
    out << "Mole " << QCoreApplication::applicationVersion() << Qt::endl;
    if (!listPlugins)
        return 0;

    out << "plugin search path:" << Qt::endl;
    for (const QString& path : mole::PluginManager::defaultSearchPaths())
        out << "  " << path << Qt::endl;

    out << "loaded plugins:" << Qt::endl;
    for (const QString& line : controller.pluginSummary())
        out << "  " << line << Qt::endl;

    const QStringList problems = controller.pluginErrors();
    if (!problems.isEmpty()) {
        out << "problems:" << Qt::endl;
        for (const QString& line : problems)
            out << "  " << line << Qt::endl;
    }
    return problems.isEmpty() ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setOrganizationDomain(QStringLiteral("io.github.ogefest"));
    app.setApplicationName(QStringLiteral("Mole"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    // Before anything else can complain. A crash takes the terminal's
    // scrollback with it, and the lines printed just before the fall are the
    // ones worth reading.
    const QString logPath = mole::sessionLog::install();
    if (!logPath.isEmpty())
        qInfo("Logging this session to %s", qPrintable(logPath));

    // Extra detail only when it was asked for. What it records goes to the same
    // file, so a report about a copy that went wrong is one file to send.
    const QStringList loud = mole::diagnostics::applyEnvironment();
    if (!loud.isEmpty())
        qInfo("Recording in detail: %s", qPrintable(loud.join(QStringLiteral(", "))));

    // Value types have to be known to the meta-object system before any task
    // can ship one across a thread boundary.
    mole::registerCoreMetaTypes();

    QQuickStyle::setStyle(QStringLiteral("Material"));

    mole::AppController controller;

    // The application decides what ships in the box; everything else is found
    // on disk. Both go through the identical registration path.
    std::vector<std::unique_ptr<mole::IPlugin>> builtIns;
    builtIns.push_back(
        std::make_unique<mole::BuiltinPlugin>(mole::VfsUri::fromLocalPath(QDir::homePath()).toString()));

    QString error;
    if (!controller.initialise(std::move(builtIns), &error)) {
        qCritical("Mole could not start: %s", qPrintable(error));
        return 1;
    }

    const QStringList arguments = app.arguments();
    if (arguments.contains(QStringLiteral("--version")) || arguments.contains(QStringLiteral("--plugins"))) {
        return runDiagnostics(controller, arguments.contains(QStringLiteral("--plugins")));
    }

    QQmlApplicationEngine engine;
    // Exposed as a context property rather than a registered singleton so the
    // QML side stays free of C++ type registration -- QML reaches every
    // property through the meta-object system anyway.
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);

    // Qt 6.4 has no loadFromModule(); the explicit resource url is the
    // portable spelling until the baseline moves to 6.5.
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/Main.qml")));

    if (engine.rootObjects().isEmpty()) {
        qCritical("Mole could not load its user interface");
        return 1;
    }

    const int code = app.exec();

    // Closed before the engine and the plugins come down, so a message emitted
    // during teardown still reaches a file that is open to receive it.
    mole::sessionLog::shutdown();
    return code;
}
