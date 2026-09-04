#include "support/MoleTestMain.h"
#include "support/OfferingFileSystem.h"
#include "support/TestSupport.h"

#include "core/tasks/InvokeFileActionTask.h"
#include "core/tasks/QueryAccessTask.h"
#include "core/tasks/QueryFileActionsTask.h"
#include "core/tasks/QueryFolderActionsTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QSignalSpy>

using namespace mole;
using namespace mole::test;

namespace {

/// The parts of a drive neither of the two drives below has an opinion about.
///
/// A memory drive underneath and nothing else: what each of them is written to
/// get wrong is one method, and everything around that method has to work or
/// the task never reaches it.
class PlainDrive : public IFileSystem
{
public:
    explicit PlainDrive(std::shared_ptr<MemoryFileSystem> inner)
        : m_inner(std::move(inner))
    {
    }

    QString scheme() const override { return m_inner->scheme(); }
    VfsCapabilities capabilities() const override { return m_inner->capabilities(); }
    Result<FileEntryList> list(const VfsUri& dir, const CancelToken& cancel) override
    {
        return m_inner->list(dir, cancel);
    }
    Result<FileEntry> stat(const VfsUri& target) override { return m_inner->stat(target); }
    Result<std::unique_ptr<QIODevice>> openRead(
        const VfsUri& target, qint64 expectedSize, const CancelToken& cancel) override
    {
        return m_inner->openRead(target, expectedSize, cancel);
    }

protected:
    const std::shared_ptr<MemoryFileSystem>& memory() const { return m_inner; }

private:
    std::shared_ptr<MemoryFileSystem> m_inner;
};

/// A drive that says it did something and hands back nothing to show for it.
/// There is no honest backend to borrow this from, which is the point: the case
/// only ever arrives from a drive that is wrong, so the drive has to be written.
class SilentDrive final : public PlainDrive
{
public:
    using PlainDrive::PlainDrive;

    Result<FileActionOutcome> invoke(const QString&, const VfsUri&, const CancelToken&) override
    {
        return FileActionOutcome::fromText(QString());
    }
};

/// A drive that reports access, in the two ways a real one does: with an answer,
/// and with a reason it has none. Both are ordinary -- a bucket knows, an FTP
/// server does not -- and neither is a task failure.
class AccessDrive final : public PlainDrive
{
public:
    using PlainDrive::PlainDrive;

    void refuse(VfsError::Code code, const QString& why)
    {
        m_code = code;
        m_why = why;
    }

    VfsCapabilities capabilities() const override
    {
        return PlainDrive::capabilities() | VfsCapability::ReportsAccess;
    }

    Result<AccessInfo> access(const VfsUri& target) override
    {
        ++m_asked;
        if (m_code != VfsError::None)
            return VfsError::make(m_code, m_why);
        AccessInfo info;
        info.read = AccessInfo::Answer::Yes;
        info.write = AccessInfo::Answer::No;
        info.nativeText = QStringLiteral("r--r--r--");
        info.owner = target.fileName();
        return info;
    }

    int timesAsked() const { return m_asked; }

private:
    VfsError::Code m_code = VfsError::None;
    QString m_why;
    int m_asked = 0;
};

} // namespace

/// The four tasks that stand between the interface and a drive's own actions.
///
/// They were reachable only through BrowserPaneController, which means the rules
/// they are written to keep were only ever asserted where a view happened to
/// exercise them. Each of these is a rule stated in one of the four run()
/// bodies: a query that cannot be answered is not a failure, an invoke that
/// answers with nothing is, a cancel is honoured while the drive is inside the
/// call, and a drive that does not report access is not asked. See MOLE-401.
class TestDriveActionTasks : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_memory;
    std::shared_ptr<OfferingFileSystem> m_fs;

    static VfsUri uri(const QString& path) { return VfsUri(QStringLiteral("mem"), QString(), path); }

private slots:
    void init();
    void cleanup();

    void aRowHandedInIsNotAskedAboutAgain();
    void aRowThatIsGoneIsNotAFailure();
    void aDirectoryIsOfferedNothingRatherThanRefused();

    void oneQueryAnswersForTheWholeFolder();
    void aFolderQueryThatFailsLeavesTheListingAlone();
    void aFolderQueryIsAbandonedWhileTheDriveIsInsideIt();

    void anIdTheDriveNeverIssuedIsAFailureThatNamesIt();
    void anActionIsAbandonedWhileTheDriveIsInsideIt();
    void aDriveThatAnswersWithNothingIsAFailure();
    void anActionWithNoDriveLeftFailsRatherThanCrashes();

    void aDriveThatDoesNotReportAccessIsNotAsked();
    void accessThatCannotBeAnsweredIsNotAFailure();
    void accessThatIsAnsweredReachesWhoeverAsked();
};

void TestDriveActionTasks::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_memory = std::make_shared<MemoryFileSystem>();
    m_fs = std::make_shared<OfferingFileSystem>(m_memory);
}

void TestDriveActionTasks::cleanup()
{
    // Destroyed here rather than left to the next init(): the destructor cancels
    // and joins the pool, and a task still running then races the harness.
    m_tasks.reset();
    m_fs.reset();
    m_memory.reset();
}

void TestDriveActionTasks::aRowHandedInIsNotAskedAboutAgain()
{
    // The constructor that takes the row exists so a cursor step costs nothing,
    // and the only way to prove the round trip did not happen is to make it
    // fatal: this drive cannot stat that path at all, and the answer still comes
    // back.
    m_memory->addFile(QStringLiteral("/report.txt"), QByteArray("body"));
    m_fs->addVersion(QStringLiteral("/report.txt"), QStringLiteral("v1"), QByteArray("older"));
    m_memory->setFault(QStringLiteral("/report.txt"), VfsError::NetworkError);

    FileEntry known;
    known.name = QStringLiteral("report.txt");
    known.uri = uri(QStringLiteral("/report.txt"));
    known.size = 4;

    auto* task = new QueryFileActionsTask(m_fs, uri(QStringLiteral("/report.txt")), known);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QStringList ids;
    for (const FileAction& action : task->actions())
        ids.append(action.id);
    QVERIFY2(ids.contains(OfferingFileSystem::versionsAction()), qPrintable(ids.join(QLatin1Char(' '))));
}

void TestDriveActionTasks::aRowThatIsGoneIsNotAFailure()
{
    // Asking about a row that has been deleted since the listing is the normal
    // course of things, and lighting up the interface over it would make a
    // stale row look like a broken drive.
    auto* task = new QueryFileActionsTask(m_fs, uri(QStringLiteral("/vanished.txt")));
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->actions().isEmpty());
}

void TestDriveActionTasks::aDirectoryIsOfferedNothingRatherThanRefused()
{
    m_memory->addDirectory(QStringLiteral("/folder"));

    auto* task = new QueryFileActionsTask(m_fs, uri(QStringLiteral("/folder")));
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->actions().isEmpty());
}

void TestDriveActionTasks::oneQueryAnswersForTheWholeFolder()
{
    m_memory->addDirectory(QStringLiteral("/wide"));
    for (int index = 0; index < 12; ++index) {
        const QString path = QStringLiteral("/wide/file%1.txt").arg(index);
        m_memory->addFile(path, QByteArray("x"));
        if (index % 2 == 0)
            m_fs->setLinkable(path, false);
    }

    auto* task = new QueryFolderActionsTask(m_fs, uri(QStringLiteral("/wide")));
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->names().size(), 6);
    // The claim the folder query exists to make: one request whatever the number
    // of rows. Twelve round trips would be twelve times the latency on a drive
    // where latency is the whole cost.
    QCOMPARE(m_fs->folderQueryCallCount(), 1);
    QCOMPARE(m_fs->actionsForCallCount(), 0);
}

void TestDriveActionTasks::aFolderQueryThatFailsLeavesTheListingAlone()
{
    m_memory->addDirectory(QStringLiteral("/unreadable"));
    m_memory->setFault(QStringLiteral("/unreadable"), VfsError::AccessDenied);

    auto* task = new QueryFolderActionsTask(m_fs, uri(QStringLiteral("/unreadable")));
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    // Somebody asked for a listing and has one. A mark that cannot be worked out
    // is a mark that does not appear, not an error over the folder they opened.
    QCOMPARE(task->state(), Task::State::Succeeded);
    QVERIFY(task->names().isEmpty());
}

void TestDriveActionTasks::aFolderQueryIsAbandonedWhileTheDriveIsInsideIt()
{
    m_memory->addDirectory(QStringLiteral("/slow"));
    m_memory->addFile(QStringLiteral("/slow/one.txt"), QByteArray("x"));
    m_fs->setActionDelayMs(4000);

    auto* task = new QueryFolderActionsTask(m_fs, uri(QStringLiteral("/slow")));
    m_tasks->submit(task);
    // The drive itself says when it is inside the call. Waiting on that rather
    // than on a clock is what keeps this from passing on one machine only.
    QVERIFY(waitFor([this] { return m_fs->isWorking(); }));
    task->requestCancel();
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Cancelled);
    QVERIFY(task->names().isEmpty());
}

void TestDriveActionTasks::anIdTheDriveNeverIssuedIsAFailureThatNamesIt()
{
    m_memory->addFile(QStringLiteral("/report.txt"), QByteArray("body"));

    auto* task = new InvokeFileActionTask(m_fs, QStringLiteral("org.mole.test.invented"),
        QStringLiteral("Do the invented thing"), uri(QStringLiteral("/report.txt")));
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    // The shell and the drive disagreeing about what is on offer, and the id is
    // the only thing that says where the disagreement is -- so it has to be in
    // the message rather than only in the log.
    QCOMPARE(task->state(), Task::State::Failed);
    QCOMPARE(task->error().code, VfsError::NotSupported);
    QVERIFY2(task->error().message.contains(QStringLiteral("org.mole.test.invented")),
        qPrintable(task->error().message));
    QVERIFY(!task->outcome().isValid());
}

void TestDriveActionTasks::anActionIsAbandonedWhileTheDriveIsInsideIt()
{
    m_memory->addFile(QStringLiteral("/report.txt"), QByteArray("body"));
    m_fs->addVersion(QStringLiteral("/report.txt"), QStringLiteral("v1"), QByteArray("older"));
    m_fs->setActionDelayMs(4000);

    auto* task = new InvokeFileActionTask(m_fs, OfferingFileSystem::versionsAction(),
        QStringLiteral("Earlier versions"), uri(QStringLiteral("/report.txt")));
    m_tasks->submit(task);
    QVERIFY(waitFor([this] { return m_fs->isWorking(); }));
    task->requestCancel();
    QVERIFY(waitForTask(task));

    // Cancelled rather than Failed: somebody who changed their mind has not been
    // told their drive is broken.
    QCOMPARE(task->state(), Task::State::Cancelled);
    QVERIFY(!task->outcome().isValid());
}

void TestDriveActionTasks::aDriveThatAnswersWithNothingIsAFailure()
{
    auto silent = std::make_shared<SilentDrive>(m_memory);
    m_memory->addFile(QStringLiteral("/report.txt"), QByteArray("body"));

    auto* task = new InvokeFileActionTask(silent, OfferingFileSystem::linkAction(),
        QStringLiteral("Copy a temporary link"), uri(QStringLiteral("/report.txt")));
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    // Caught here rather than left to whatever draws the answer, which would put
    // an empty box in front of somebody and call it a success.
    QCOMPARE(task->state(), Task::State::Failed);
    QVERIFY(!task->outcome().isValid());
}

void TestDriveActionTasks::anActionWithNoDriveLeftFailsRatherThanCrashes()
{
    // The drive was unmounted between the menu opening and the item being
    // picked. Every one of the four takes a shared pointer that can be empty by
    // the time run() happens.
    auto* invoke = new InvokeFileActionTask(
        nullptr, OfferingFileSystem::linkAction(), QStringLiteral("Link"), uri(QStringLiteral("/gone.txt")));
    m_tasks->submit(invoke);
    QVERIFY(waitForTask(invoke));
    QCOMPARE(invoke->state(), Task::State::Failed);

    auto* access = new QueryAccessTask(nullptr, uri(QStringLiteral("/gone.txt")));
    m_tasks->submit(access);
    QVERIFY(waitForTask(access));
    QCOMPARE(access->state(), Task::State::Failed);

    // The two queries are background work nobody asked for, so they go quiet
    // rather than failing where a query would be reported.
    auto* file = new QueryFileActionsTask(nullptr, uri(QStringLiteral("/gone.txt")));
    m_tasks->submit(file);
    QVERIFY(waitForTask(file));
    QCOMPARE(file->state(), Task::State::Succeeded);
    QVERIFY(file->actions().isEmpty());

    auto* folder = new QueryFolderActionsTask(nullptr, uri(QStringLiteral("/gone")));
    m_tasks->submit(folder);
    QVERIFY(waitForTask(folder));
    QCOMPARE(folder->state(), Task::State::Succeeded);
    QVERIFY(folder->names().isEmpty());
}

void TestDriveActionTasks::aDriveThatDoesNotReportAccessIsNotAsked()
{
    m_memory->addFile(QStringLiteral("/report.txt"), QByteArray("body"));

    auto* task = new QueryAccessTask(m_fs, uri(QStringLiteral("/report.txt")));
    QSignalSpy ready(task, &QueryAccessTask::accessReady);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(ready.count(), 0);
    QVERIFY2(task->statusText().contains(QStringLiteral("does not report")), qPrintable(task->statusText()));
}

void TestDriveActionTasks::accessThatCannotBeAnsweredIsNotAFailure()
{
    auto drive = std::make_shared<AccessDrive>(m_memory);
    m_memory->addFile(QStringLiteral("/report.txt"), QByteArray("body"));
    drive->refuse(VfsError::NetworkError, QStringLiteral("the server closed the connection"));

    auto* task = new QueryAccessTask(drive, uri(QStringLiteral("/report.txt")));
    QSignalSpy ready(task, &QueryAccessTask::accessReady);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    // Plenty of drives that advertise access cannot answer about a given path.
    // Recording that as a failed task would light up the interface over a panel
    // nobody may even have open, so the reason goes in the line instead.
    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(ready.count(), 0);
    QCOMPARE(drive->timesAsked(), 1);
    QVERIFY2(
        task->statusText().contains(QStringLiteral("closed the connection")), qPrintable(task->statusText()));
}

void TestDriveActionTasks::accessThatIsAnsweredReachesWhoeverAsked()
{
    auto drive = std::make_shared<AccessDrive>(m_memory);
    m_memory->addFile(QStringLiteral("/report.txt"), QByteArray("body"));

    auto* task = new QueryAccessTask(drive, uri(QStringLiteral("/report.txt")));
    QSignalSpy ready(task, &QueryAccessTask::accessReady);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(ready.count(), 1);
    const auto info = ready.first().at(1).value<AccessInfo>();
    QCOMPARE(info.read, AccessInfo::Answer::Yes);
    QCOMPARE(info.write, AccessInfo::Answer::No);
    QCOMPARE(info.nativeText, QStringLiteral("r--r--r--"));
    // The native form is what a properties panel shows as-is, so it is also what
    // the task's own line says rather than a sentence assembled from the flags.
    QCOMPARE(task->statusText(), QStringLiteral("r--r--r--"));
}

MOLE_TEST_MAIN(TestDriveActionTasks)
#include "tst_DriveActionTasks.moc"
