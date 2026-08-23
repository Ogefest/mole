#include "TestSupport.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QThread>

#include <utility>

#ifdef Q_OS_UNIX
#include <utime.h>
#endif

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
        { "MOLE_PREFERENCES_PATH", "preferences.json" },
        { "MOLE_SECRETS_PATH", "credentials.enc" },
        { "MOLE_REMOTES_PATH", "drives.json" },
        // Recomputable, unlike the ten above, and still never the real one: a test
        // that writes into the developer's own thumbnail cache is a test that
        // changes the machine it runs on.
        { "MOLE_THUMBNAILS_PATH", "thumbnails" },
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

bool refreshIndexSummary(IndexSummary* summary, int timeoutMs)
{
    if (!summary)
        return false;

    // Waiting for the next `changed` is not enough: a read already in flight
    // answers with the state from before this call, and the coalesced repeat is
    // the one that carries what was just written. So wait for a read to land
    // *and* for none to be in flight -- which is one read when the summary was
    // idle and two when it was not.
    const qint64 before = summary->reads();
    summary->refresh();
    return waitFor(
        [summary, before] { return summary->reads() > before && !summary->isReading(); }, timeoutMs);
}

void drainEvents()
{
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

bool setModifiedTime(const QString& absolutePath, const QDateTime& when)
{
    if (QFileInfo(absolutePath).isDir()) {
#ifdef Q_OS_UNIX
        const time_t stamp = static_cast<time_t>(when.toSecsSinceEpoch());
        utimbuf times { stamp, stamp };
        return utime(QFile::encodeName(absolutePath).constData(), &times) == 0;
#else
        return false;
#endif
    }

    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    return file.setFileTime(when, QFileDevice::FileModificationTime);
}

bool madeUnreadable(const QString& absolutePath)
{
    if (!QFile::setPermissions(absolutePath, {}))
        return false;
    QFile probe(absolutePath);
    if (!probe.open(QIODevice::ReadOnly))
        return true;
    probe.close();
    QFile::setPermissions(absolutePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return false;
}

TempTree::TempTree() = default;

bool TempTree::setModified(const QString& relativePath, const QDateTime& when)
{
    return setModifiedTime(relativePath.isEmpty() ? m_dir.path() : absolute(relativePath), when);
}

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

namespace {

    // Qt's message handler is a free function with no room for context, so the
    // collected lines live here. Guarded because a task logs from the pool
    // thread while the test reads from its own.
    QMutex g_captureMutex;
    QStringList g_captured;
    QtMessageHandler g_capturePrevious = nullptr;
    bool g_capturing = false;
    QtMsgType g_captureFrom = QtWarningMsg;

    /// Quiet to loud, which QtMsgType's own values are not: debug is 0, warning 1,
    /// critical 2, fatal 3 and **info 4**, added later and numbered last. Comparing
    /// the enum directly would make "info and above" mean "info and fatal".
    int loudness(QtMsgType type)
    {
        switch (type) {
        case QtDebugMsg:
            return 0;
        case QtInfoMsg:
            return 1;
        case QtWarningMsg:
            return 2;
        case QtCriticalMsg:
            return 3;
        case QtFatalMsg:
            return 4;
        }
        return 0;
    }

    void captureHandler(QtMsgType type, const QMessageLogContext& context, const QString& text)
    {
        // Fatal is never captured: the process has to die on one, and swallowing it
        // would turn a crash into a test that hangs.
        if (type != QtFatalMsg && loudness(type) >= loudness(g_captureFrom)) {
            const QMutexLocker lock(&g_captureMutex);
            g_captured.append(text);
            return;
        }
        // Everything quieter goes through, so a test run stays readable.
        if (g_capturePrevious)
            g_capturePrevious(type, context, text);
    }

} // namespace

CapturedWarnings::CapturedWarnings(QtMsgType from)
{
    // Nesting two of these would leave the inner one's handler installed when
    // the outer one restored, so say so rather than mislead a later test.
    Q_ASSERT_X(!g_capturing, "CapturedWarnings", "already capturing warnings in this test");
    {
        const QMutexLocker lock(&g_captureMutex);
        g_captured.clear();
    }
    g_capturing = true;
    g_captureFrom = from;
    m_previous = qInstallMessageHandler(captureHandler);
    g_capturePrevious = m_previous;
}

CapturedWarnings::~CapturedWarnings()
{
    qInstallMessageHandler(m_previous);
    g_capturePrevious = nullptr;
    g_capturing = false;
    g_captureFrom = QtWarningMsg;
}

QStringList CapturedWarnings::messages() const
{
    const QMutexLocker lock(&g_captureMutex);
    return g_captured;
}

bool CapturedWarnings::contains(const QString& needle) const
{
    const QMutexLocker lock(&g_captureMutex);
    for (const QString& line : std::as_const(g_captured)) {
        if (line.contains(needle))
            return true;
    }
    return false;
}

QString CapturedWarnings::joined() const
{
    return messages().join(QStringLiteral(" | "));
}

} // namespace mole::test
