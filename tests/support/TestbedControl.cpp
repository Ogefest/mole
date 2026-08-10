#include "TestbedControl.h"

#include <QProcess>

namespace mole::test {
namespace {

    /// The command that reaches the machine, split the way a shell would.
    /// Empty when nothing named one, which is the ordinary case.
    QStringList prefix()
    {
        const QString value = QString::fromLocal8Bit(qgetenv("MOLE_TEST_CONTROL")).trimmed();
        if (value.isEmpty())
            return {};
        return value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }

} // namespace

bool TestbedControl::isAvailable()
{
    return !prefix().isEmpty();
}

QString TestbedControl::run(const QStringList& arguments, int timeoutMs)
{
    QStringList command = prefix();
    if (command.isEmpty())
        return {};

    const QString program = command.takeFirst();
    command += arguments;

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, command);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        return QStringLiteral("control channel timed out running: %1").arg(arguments.join(QLatin1Char(' ')));
    }
    return QString::fromLocal8Bit(process.readAll()).trimmed();
}

QString TestbedControl::restore()
{
    if (!isAvailable())
        return {};
    return run({ QStringLiteral("restore") });
}

} // namespace mole::test
