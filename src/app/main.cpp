#include "app/SessionLog.h"
#include "host/PluginManager.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "ui/AppController.h"
#include "ui/DragSource.h"

#include "core/CoreMetaTypes.h"
#include "core/diagnostics/Diagnostics.h"

#include <QDir>
#include <QDrag>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
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

/// What the pointer carries while a drag of more than one file is in flight.
///
/// A cursor with nothing attached to it says nothing about how much is going, and
/// the difference between dragging one file and dragging forty is exactly what
/// somebody wants confirmed before they let go over another window.
QPixmap countBadge(int count)
{
    const QString text = QStringLiteral("%1 items").arg(count);
    QFont font;
    font.setPixelSize(13);
    const QFontMetrics metrics(font);
    const int width = metrics.horizontalAdvance(text) + 18;
    const int height = metrics.height() + 10;

    QPixmap badge(width, height);
    badge.fill(Qt::transparent);
    QPainter painter(&badge);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(0x2b, 0x35, 0x47));
    painter.setPen(QColor(0x4a, 0x53, 0x64));
    painter.drawRoundedRect(QRectF(0.5, 0.5, width - 1, height - 1), 3, 3);
    painter.setFont(font);
    painter.setPen(QColor(0xe6, 0xeb, 0xf5));
    painter.drawText(QRect(0, 0, width, height), Qt::AlignCenter, text);
    return badge;
}

/// The one call this application makes to the platform's drag machinery.
///
/// Everything that is Mole's own behaviour -- which rows go, what the payload
/// holds, which action is offered -- happened before this and is tested without a
/// platform. See ADR-0040 for why the seam is here.
void installDragHook(mole::AppController& controller, QQuickWindow* window)
{
    mole::DragSource* source = controller.dragSource();
    if (!source || !window)
        return;

    source->setStartHook([window](std::unique_ptr<QMimeData> mime, Qt::DropActions actions) {
        const int count = static_cast<int>(mime->urls().size());
        // Parented to the window, which is also what the receiving application
        // is told the drag came from.
        auto* drag = new QDrag(window);
        drag->setMimeData(mime.release());
        if (count > 1)
            drag->setPixmap(countBadge(count));
        // Blocks until the gesture ends, which is what QDrag is: the nested loop
        // is the drag.
        drag->exec(actions);
        return true;
    });
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

    // Once there is a window: a QDrag needs a real one as its source.
    installDragHook(controller, qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst()));

    const int code = app.exec();

    // Closed before the engine and the plugins come down, so a message emitted
    // during teardown still reaches a file that is open to receive it.
    mole::sessionLog::shutdown();
    return code;
}
