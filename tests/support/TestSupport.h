#pragma once

#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace mole::test {

/// Spins the event loop until `task` reaches a terminal state. Tasks report
/// their state through queued invocations, so a plain sleep would never see
/// the update -- always use this instead.
bool waitForTask(Task* task, int timeoutMs = 10000);

/// Spins the event loop until `predicate` holds or the timeout expires.
bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 5000);

/// Drains everything currently queued, including deleteLater().
void drainEvents();

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
    QString absolute(const QString& relativePath) const;

private:
    QTemporaryDir m_dir;
};

/// Collects a task's error without the test having to reach into internals.
QString errorTextOf(const Task& task);

} // namespace mole::test
