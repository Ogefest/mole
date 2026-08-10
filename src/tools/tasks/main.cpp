#include "tools/tasks/Commands.h"
#include "tools/tasks/ToolEnvironment.h"

#include "core/CoreMetaTypes.h"
#include "core/diagnostics/Diagnostics.h"

#include <QCoreApplication>
#include <QTextStream>

/// `mole-tasks` -- every Mole task, from a console, with no window.
///
/// A QCoreApplication rather than a QGuiApplication, and no QML anywhere: this
/// binary has to run on a machine with no display, under `tc netem`, and inside
/// a loop in a shell script. See docs/adr/0028-a-console-runner-for-the-tasks.md.
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("Mole"));
    app.setOrganizationDomain(QStringLiteral("io.github.ogefest"));
    // The same application name as the window, deliberately: the drives, the
    // credentials and the index are the ones the user already configured, and a
    // second name would quietly give this tool a second, empty configuration.
    app.setApplicationName(QStringLiteral("Mole"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));

    // MOLE_LOG applies before anything is built, exactly as it does for the
    // window. `--log` is read later and adds to it.
    mole::diagnostics::applyEnvironment();

    // Value types have to be known to the meta-object system before any task
    // can ship one across a thread boundary.
    mole::registerCoreMetaTypes();

    QTextStream out(stdout);
    QTextStream err(stderr);

    mole::tools::ToolEnvironment environment;
    return mole::tools::runMoleTasks(app.arguments().mid(1), environment, out, err);
}
