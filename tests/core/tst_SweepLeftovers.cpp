#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/tasks/SweepLeftoversTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/IFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDateTime>

using namespace mole;
using namespace mole::test;

namespace {

/// A drive holding things no listing shows.
///
/// The S3 case in miniature, and deliberately not S3: what is being tested is
/// the task and the rule about age, neither of which knows what a multipart
/// upload is. A backend that reports leftovers of some other kind gets the same
/// behaviour for free, which is the point of putting this on the drive.
class DriveWithLeftovers final : public IFileSystem
{
public:
    QString scheme() const override { return QStringLiteral("held"); }
    VfsCapabilities capabilities() const override
    {
        return VfsCapability::Read | VfsCapability::ReportsLeftovers;
    }
    Result<FileEntryList> list(const VfsUri&, const CancelToken&) override
    {
        return Result<FileEntryList>(FileEntryList {});
    }
    Result<FileEntry> stat(const VfsUri&) override
    {
        return Result<FileEntry>::failure(VfsError::NotFound, QStringLiteral("nothing here"));
    }

    Result<QList<DriveLeftover>> leftovers(std::chrono::seconds olderThan, const CancelToken&) override
    {
        ++listed;
        const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-olderThan.count());
        QList<DriveLeftover> out;
        for (const DriveLeftover& held : std::as_const(m_held)) {
            if (!held.started.isValid() || held.started <= cutoff)
                out.append(held);
        }
        return Result<QList<DriveLeftover>>(out);
    }

    Result<void> discardLeftover(const DriveLeftover& leftover) override
    {
        if (refuse.contains(leftover.handle))
            return Result<void>::failure(VfsError::AccessDenied, QStringLiteral("not yours"));
        for (qsizetype i = 0; i < m_held.size(); ++i) {
            if (m_held.at(i).handle == leftover.handle) {
                m_held.removeAt(i);
                return {};
            }
        }
        return Result<void>::failure(VfsError::NotFound, QStringLiteral("gone already"));
    }

    void hold(const QString& handle, int ageHours)
    {
        DriveLeftover leftover;
        leftover.handle = handle;
        leftover.path = QStringLiteral("/") + handle;
        leftover.started = QDateTime::currentDateTimeUtc().addSecs(-3600LL * ageHours);
        leftover.what = QStringLiteral("an upload that was never finished");
        m_held.append(leftover);
    }

    int remaining() const { return static_cast<int>(m_held.size()); }

    int listed = 0;
    QStringList refuse;

private:
    QList<DriveLeftover> m_held;
};

} // namespace

/// Clearing up after a copy nobody was alive to finish.
class TestSweepLeftovers : public QObject
{
    Q_OBJECT

private slots:
    void aDriveThatKeepsNothingBackSaysSoRatherThanFailing();
    void whatIsFoundIsReportedAndNotThrownAway();
    void theAgeThresholdHidesAnUploadThatMayStillBeRunning();
    void discardingSaysHowManyAndWhichOnesRefused();
};

void TestSweepLeftovers::aDriveThatKeepsNothingBackSaysSoRatherThanFailing()
{
    // Most drives have nothing of this kind. A local disk finishes its writes or
    // leaves a partial file somebody can see, so there is nothing invisible to
    // clear up -- and saying that plainly beats an error to interpret.
    TaskManager tasks;
    auto* task = new SweepLeftoversTask(
        QStringLiteral("Photos"), std::make_shared<MemoryFileSystem>(), std::chrono::hours(24), false);
    tasks.submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->state(), Task::State::Succeeded);
    QCOMPARE(task->found().size(), 0);
    QVERIFY2(task->summary().contains(QStringLiteral("nothing")), qPrintable(task->summary()));
}

void TestSweepLeftovers::whatIsFoundIsReportedAndNotThrownAway()
{
    // Finding and removing are two steps on purpose. What a sweep finds is
    // somebody's, and one of them may be a copy running on another machine right
    // now -- so the first pass reports and touches nothing.
    TaskManager tasks;
    auto drive = std::make_shared<DriveWithLeftovers>();
    drive->hold(QStringLiteral("holiday.raw"), 48);
    drive->hold(QStringLiteral("backup.img"), 72);

    auto* task = new SweepLeftoversTask(QStringLiteral("Bucket"), drive, std::chrono::hours(24), false);
    tasks.submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->found().size(), 2);
    QCOMPARE(task->discardedCount(), 0);
    QCOMPARE(drive->remaining(), 2);
    QVERIFY2(task->summary().contains(QStringLiteral("2")), qPrintable(task->summary()));
}

void TestSweepLeftovers::theAgeThresholdHidesAnUploadThatMayStillBeRunning()
{
    // The rule that keeps this from breaking a copy that is going perfectly
    // well. An upload begun a minute ago looks exactly like one a killed process
    // left behind, and abandoning it would be a worse fault than the one being
    // cleaned up.
    TaskManager tasks;
    auto drive = std::make_shared<DriveWithLeftovers>();
    drive->hold(QStringLiteral("started-just-now.bin"), 0);
    drive->hold(QStringLiteral("left-days-ago.bin"), 96);

    auto* task = new SweepLeftoversTask(QStringLiteral("Bucket"), drive, std::chrono::hours(24), true);
    tasks.submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->found().size(), 1);
    QCOMPARE(task->discardedCount(), 1);
    // The young one is still there, untouched.
    QCOMPARE(drive->remaining(), 1);
}

void TestSweepLeftovers::discardingSaysHowManyAndWhichOnesRefused()
{
    // A sweep that cleared three of four and said "done" would be a sweep that
    // leaves somebody paying for the fourth without knowing.
    TaskManager tasks;
    auto drive = std::make_shared<DriveWithLeftovers>();
    drive->hold(QStringLiteral("one.bin"), 48);
    drive->hold(QStringLiteral("two.bin"), 48);
    drive->refuse = { QStringLiteral("two.bin") };

    auto* task = new SweepLeftoversTask(QStringLiteral("Bucket"), drive, std::chrono::hours(24), true);
    tasks.submit(task);
    QVERIFY(waitForTask(task));

    QCOMPARE(task->discardedCount(), 1);
    QVERIFY2(task->summary().contains(QStringLiteral("two.bin")), qPrintable(task->summary()));
    QVERIFY2(task->summary().contains(QStringLiteral("not yours")), qPrintable(task->summary()));
    QCOMPARE(drive->remaining(), 1);
}

MOLE_TEST_MAIN(TestSweepLeftovers)
#include "tst_SweepLeftovers.moc"
