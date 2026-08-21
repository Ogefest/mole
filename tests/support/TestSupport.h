#pragma once

#include "core/index/IndexSummary.h"
#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>

namespace mole::test {

/// A task whose body is supplied by the test. Shared, because "a task that takes
/// a measurable amount of time and reports as it goes" is what several suites
/// need and none of them should have to grow their own.
class ScriptedTask final : public Task
{
public:
    using Body = std::function<void(ScriptedTask&)>;

    ScriptedTask(QString title, Body body)
        : Task(std::move(title))
        , m_body(std::move(body))
    {
    }

    /// Re-exposed so the test can drive them from inside the body.
    using Task::fail;
    /// A scripted task can also say which drives it touches and whether it is the
    /// application's own housekeeping -- the two things that decide whether a
    /// drive reads as busy. See DriveListModel::refreshBusyDrives().
    using Task::isCancelRequested;
    using Task::noteTouching;
    using Task::reportBytes;
    using Task::reportCount;
    using Task::reportText;
    using Task::setBackground;
    using Task::setBytesDone;
    using Task::setByteTotal;
    using Task::setOneOfMany;
    using Task::setProgress;
    using Task::setStatusText;

    QThread* ranOn() const { return m_ranOn; }

protected:
    void run() override
    {
        m_ranOn = QThread::currentThread();
        if (m_body)
            m_body(*this);
    }

private:
    Body m_body;
    std::atomic<QThread*> m_ranOn { nullptr };
};

/// Spins the event loop until `task` reaches a terminal state. Tasks report
/// their state through queued invocations, so a plain sleep would never see
/// the update -- always use this instead.
bool waitForTask(Task* task, int timeoutMs = 10000);

/// Spins the event loop until `predicate` holds or the timeout expires.
bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 5000);

/// Drains everything currently queued, including deleteLater().
void drainEvents();

/// Refreshes the interface's snapshot of the index and waits for the answer.
///
/// The interface reads `IndexSummary` rather than the database -- ADR-0066 -- so
/// a test that writes rows by hand has written them somewhere nothing is looking
/// yet. In the application a finished scan posts `EventBus::indexUpdated` and the
/// snapshot refreshes on it; a hand-made seed has no scan, so it asks here and
/// waits for the read to come back from the pool thread.
///
/// Returns false if no answer arrives, so a test that forgets to check fails on
/// its own assertion rather than on a mystery empty list.
bool refreshIndexSummary(IndexSummary* summary, int timeoutMs = 5000);

/// Reads the index directly from the test's own thread on purpose.
///
/// ADR-0066 has `AppController` name the thread that draws the window, and a read
/// of the index from it warns -- because interface code must ask `IndexSummary`
/// instead. A test asserting about *storage* rather than about the interface is
/// the exception, and saying so here keeps the warning meaning what it says.
///
/// Only for suites that get their index through AppController; one that owns its
/// own IndexDatabase was never guarded.
class ReadingTheIndexOnPurpose
{
public:
    explicit ReadingTheIndexOnPurpose(IndexDatabase* index)
        : m_index(index)
    {
        if (m_index)
            m_index->doNotReadFrom(nullptr);
    }

    ~ReadingTheIndexOnPurpose()
    {
        if (m_index)
            m_index->doNotReadFrom(QThread::currentThread());
    }

    ReadingTheIndexOnPurpose(const ReadingTheIndexOnPurpose&) = delete;
    ReadingTheIndexOnPurpose& operator=(const ReadingTheIndexOnPurpose&) = delete;

private:
    IndexDatabase* m_index = nullptr;
};

/// Points every application store at a throwaway directory.
///
/// This is not merely tidiness. AppController opens the index, the session, the
/// bookmarks, the analysis history, the schedule and the alerts, and it *starts
/// the scheduler* -- so a test that inherits the real profile would run the
/// user's own scheduled jobs against the user's own folders. Setting each
/// variable by hand in each test is how one of them gets forgotten, so nothing
/// does that any more.
class PrivateProfile
{
public:
    PrivateProfile();
    ~PrivateProfile();

    bool isValid() const;
    QString path() const;
    /// The file or directory a given store was pointed at.
    QString filePath(const QString& name) const;

    /// Deletes the session, schedule and alert files, for a test that wants a
    /// fresh application without a fresh profile.
    void clearVolatileState() const;

private:
    std::unique_ptr<QTemporaryDir> m_dir;
};

/// Dates a file *or a folder* on the local disk.
///
/// A directory cannot be opened as a QFile, and `QFileDevice::setFileTime` is
/// the only thing Qt offers, so folders go through `utime()`. Tests need this
/// because an incremental scan is entirely about a folder's date: a tree created
/// a moment ago is one nothing can tell apart from a tree changed a moment ago,
/// and the index only trusts a folder that was already settled when the last
/// scan ran.
bool setModifiedTime(const QString& absolutePath, const QDateTime& when);

/// A temporary directory that cleans itself up, plus helpers to populate it.
class TempTree
{
public:
    TempTree();

    bool isValid() const { return m_dir.isValid(); }
    QString path() const { return m_dir.path(); }
    VfsUri rootUri() const;

    /// Creates `relativePath` and all missing parents.
    bool makeDirs(const QString& relativePath);
    /// Creates a file with the given contents, creating parents as needed.
    bool writeFile(const QString& relativePath, const QByteArray& contents = "x");
    /// Dates an entry, folders included. See setModifiedTime().
    bool setModified(const QString& relativePath, const QDateTime& when);
    QString absolute(const QString& relativePath) const;

private:
    QTemporaryDir m_dir;
};

/// Collects a task's error without the test having to reach into internals.
QString errorTextOf(const Task& task);

/// Collects the warnings the code under test logs, so a test can assert on
/// them.
///
/// Two kinds of claim need this. One is that something noisy has gone quiet --
/// a routine event that used to leave a warning behind, where the only evidence
/// is the absence of a line. The other is the opposite: that a real failure
/// still says so, which is what stops the first fix from being "log nothing".
///
/// Warnings and worse by default; everything below goes on to the handler that
/// was installed before, so ordinary test output is unaffected. The handler is
/// removed in the destructor, and messages logged from a pool thread are counted
/// safely.
///
/// Pass a lower `from` to see the quieter levels. `QtInfoMsg` is what a claim
/// about the *session log* needs: ADR-0064 put a task's start and end at info, so
/// the assertion "this ran and left a line" and its opposite "this one left none"
/// are both about lines this class could not see before. Anything at or above
/// `from` is captured and does not reach the previous handler; anything below is
/// passed through untouched.
class CapturedWarnings
{
public:
    /// `from` is the quietest level to capture. The default keeps every existing
    /// test reading exactly as it did.
    explicit CapturedWarnings(QtMsgType from = QtWarningMsg);
    ~CapturedWarnings();

    CapturedWarnings(const CapturedWarnings&) = delete;
    CapturedWarnings& operator=(const CapturedWarnings&) = delete;

    /// Everything captured since construction, in the order it was logged.
    QStringList messages() const;
    /// Whether any captured line contains `needle`.
    bool contains(const QString& needle) const;
    /// The captured lines joined for a failure message.
    QString joined() const;

private:
    QtMessageHandler m_previous = nullptr;
};

} // namespace mole::test
