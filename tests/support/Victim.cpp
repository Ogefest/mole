#include "support/Victim.h"

#include <QCoreApplication>
#include <QTest>

namespace mole::test {
namespace {

    const char* const kInstruction = "MOLE_TEST_VICTIM";

} // namespace

bool Victim::isThisProcess()
{
    return !qEnvironmentVariableIsEmpty(kInstruction);
}

QString Victim::instruction()
{
    return qEnvironmentVariable(kInstruction);
}

Victim::Victim(const QString& testFunction, const QString& instruction)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(kInstruction), instruction);

    m_process.setProcessEnvironment(environment);
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    m_process.start(QCoreApplication::applicationFilePath(), { testFunction });
    m_started = m_process.waitForStarted(10000);
}

Victim::~Victim()
{
    kill();
}

bool Victim::waitUntil(const std::function<bool()>& condition, int attempts, int gapMs)
{
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (condition())
            return true;
        // A victim that has exited is one that will never satisfy the
        // condition, and waiting out the remaining attempts only delays a
        // failure that has already happened.
        if (m_process.state() != QProcess::Running)
            return condition();
        QTest::qWait(gapMs);
    }
    return condition();
}

void Victim::kill()
{
    if (m_process.state() == QProcess::NotRunning)
        return;
    m_transcript += QString::fromLocal8Bit(m_process.readAll());
    m_process.kill();
    m_process.waitForFinished(15000);
}

QString Victim::transcript()
{
    m_transcript += QString::fromLocal8Bit(m_process.readAll());
    return m_transcript;
}

} // namespace mole::test
