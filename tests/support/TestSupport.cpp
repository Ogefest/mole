#include "TestSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QThread>

namespace mole::test {
namespace {

    /// Every store the application opens. Adding one here is what keeps a new
    /// store from silently writing into the user's real profile during a test.
    constexpr std::pair<const char*, const char*> kProfileVariables[] = {
        { "MOLE_INDEX_PATH", "index.sqlite" },
        { "MOLE_SESSION_PATH", "session.json" },
        { "MOLE_BOOKMARKS_PATH", "bookmarks.json" },
        { "MOLE_ANALYSIS_PATH", "analysis" },
        { "MOLE_SCHEDULE_PATH", "schedule.json" },
        { "MOLE_ALERTS_PATH", "alerts.json" },
        { "MOLE_SETS_PATH", "sets.json" },
        { "MOLE_SECRETS_PATH", "credentials.enc" },
        { "MOLE_REMOTES_PATH", "drives.json" },
    };

} // namespace

PrivateProfile::PrivateProfile()
    : m_dir(std::make_unique<QTemporaryDir>())
{
    if (!m_dir->isValid())
        return;
    for (const auto& [key, name] : kProfileVariables)
        qputenv(key, QDir(m_dir->path()).filePath(QLatin1String(name)).toLocal8Bit());
}

PrivateProfile::~PrivateProfile()
{
    // Unset rather than left pointing at a directory that no longer exists:
    // a stale path would send the next test somewhere unwritable.
    for (const auto& [key, name] : kProfileVariables)
        qunsetenv(key);
}

bool PrivateProfile::isValid() const
{
    return m_dir && m_dir->isValid();
}

QString PrivateProfile::path() const
{
    return m_dir ? m_dir->path() : QString();
}

QString PrivateProfile::filePath(const QString& name) const
{
    return QDir(path()).filePath(name);
}

void PrivateProfile::clearVolatileState() const
{
    QFile::remove(filePath(QStringLiteral("session.json")));
    QFile::remove(filePath(QStringLiteral("schedule.json")));
    QFile::remove(filePath(QStringLiteral("alerts.json")));
    QFile::remove(filePath(QStringLiteral("credentials.enc")));
    QFile::remove(filePath(QStringLiteral("drives.json")));
    QDir(filePath(QStringLiteral("analysis"))).removeRecursively();
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    // One last pass so a result that landed during the final sleep still counts.
    QCoreApplication::processEvents();
    return predicate();
}

bool waitForTask(Task* task, int timeoutMs)
{
    if (!task)
        return false;
    const bool finished = waitFor([task] { return task->isFinished(); }, timeoutMs);
    // Progress and status updates are queued behind the state change; flush
    // them so assertions on statusText() see the final value.
    drainEvents();
    return finished;
}

void drainEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

TempTree::TempTree() = default;

VfsUri TempTree::rootUri() const
{
    return VfsUri::fromLocalPath(m_dir.path());
}

QString TempTree::absolute(const QString& relativePath) const
{
    return QDir(m_dir.path()).filePath(relativePath);
}

bool TempTree::makeDirs(const QString& relativePath)
{
    return QDir(m_dir.path()).mkpath(relativePath);
}

bool TempTree::writeFile(const QString& relativePath, const QByteArray& contents)
{
    const QString target = absolute(relativePath);
    const QFileInfo info(target);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath()))
        return false;

    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(contents) == contents.size();
}

QString errorTextOf(const Task& task)
{
    return task.error().message;
}

} // namespace mole::test
