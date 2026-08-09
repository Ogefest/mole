#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/DriveCheckTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QSignalSpy>

using namespace mole;
using namespace mole::test;

class TestDriveCheckTask : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_fs;

    VfsUri root() const { return VfsUri(QStringLiteral("mem"), QString(), QStringLiteral("/")); }

private slots:
    void init();
    void aReachableDriveSaysWhatItFound();
    void anEmptyRootIsStillReachable();
    void anUnreachableDriveCarriesTheReason();
    void aMissingBackendIsReportedRatherThanCrashing();
    void aCancelledCheckSaysSoInsteadOfFailing();
};

void TestDriveCheckTask::init()
{
    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<MemoryFileSystem>();
}

void TestDriveCheckTask::aReachableDriveSaysWhatItFound()
{
    m_fs->addFile(QStringLiteral("/alpha.txt"), QByteArray("one"));
    m_fs->addFile(QStringLiteral("/beta.txt"), QByteArray("two"));
    m_fs->addDirectory(QStringLiteral("/nested"));

    auto* task = new DriveCheckTask(QStringLiteral("Scratch"), m_fs, root());
    QSignalSpy checked(task, &DriveCheckTask::checked);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(checked.count(), 1);
    QVERIFY2(checked.first().at(0).toBool(), "a memory drive is always reachable");
    // The count is the point: an empty answer from the wrong place looks exactly
    // like an empty answer from the right one, so the number is what tells them
    // apart.
    const QString message = checked.first().at(1).toString();
    QVERIFY2(message.contains(QStringLiteral("3")), qPrintable(message));
    QCOMPARE(task->state(), Task::State::Succeeded);
}

void TestDriveCheckTask::anEmptyRootIsStillReachable()
{
    auto* task = new DriveCheckTask(QStringLiteral("Scratch"), m_fs, root());
    QSignalSpy checked(task, &DriveCheckTask::checked);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(checked.count(), 1);
    QVERIFY(checked.first().at(0).toBool());
    QVERIFY2(checked.first().at(1).toString().contains(QStringLiteral("empty")),
        qPrintable(checked.first().at(1).toString()));
    QCOMPARE(task->state(), Task::State::Succeeded);
}

void TestDriveCheckTask::anUnreachableDriveCarriesTheReason()
{
    // The whole reason this task exists: without it, a drive that cannot be
    // reached is indistinguishable from one that can until something tries to
    // read from it, and the reason surfaces several steps away from the form
    // where it was caused.
    m_fs->setFault(QStringLiteral("/"), VfsError::NetworkError);

    auto* task = new DriveCheckTask(QStringLiteral("Broken"), m_fs, root());
    QSignalSpy checked(task, &DriveCheckTask::checked);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(checked.count(), 1);
    QVERIFY2(!checked.first().at(0).toBool(), "a faulting backend is not reachable");
    QVERIFY2(
        !checked.first().at(1).toString().isEmpty(), "the failure has to say something a reader can act on");
    QCOMPARE(task->state(), Task::State::Failed);
    QCOMPARE(task->error().code, VfsError::NetworkError);
}

void TestDriveCheckTask::aMissingBackendIsReportedRatherThanCrashing()
{
    // A factory that refused the configuration hands back nothing. Reporting it
    // is the difference between a message and a null dereference.
    auto* task = new DriveCheckTask(QStringLiteral("Nothing"), nullptr, root());
    QSignalSpy checked(task, &DriveCheckTask::checked);
    m_tasks->submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(checked.count(), 1);
    QVERIFY(!checked.first().at(0).toBool());
    QCOMPARE(task->state(), Task::State::Failed);
}

void TestDriveCheckTask::aCancelledCheckSaysSoInsteadOfFailing()
{
    // A check against a host that is not answering has to be abandonable, and
    // being abandoned is not the same as the drive being broken -- reporting it
    // as a failure would accuse a configuration that might be perfectly good.
    m_fs->setListDelayMs(400);

    auto* task = new DriveCheckTask(QStringLiteral("Slow"), m_fs, root());
    QSignalSpy checked(task, &DriveCheckTask::checked);
    m_tasks->submit(task);
    QVERIFY(waitFor([task] { return task->state() == Task::State::Running; }));
    task->requestCancel();
    QVERIFY(waitForTask(task));

    QCOMPARE(checked.count(), 1);
    QVERIFY(!checked.first().at(0).toBool());
    QVERIFY2(checked.first().at(1).toString().contains(QStringLiteral("cancel"), Qt::CaseInsensitive),
        qPrintable(checked.first().at(1).toString()));
}

MOLE_TEST_MAIN(TestDriveCheckTask)

#include "tst_DriveCheckTask.moc"
