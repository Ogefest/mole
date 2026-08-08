#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/models/MountListModel.h"

#include "core/CoreMetaTypes.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QAbstractItemModelTester>
#include <QSignalSpy>

using namespace mole;
using namespace mole::test;

namespace {

/// A drive that claims a capacity, so the sidebar has something to draw
/// without depending on whatever the machine running the tests happens to
/// have mounted.
class SizedFileSystem final : public IFileSystem
{
public:
    QString scheme() const override { return QStringLiteral("sized"); }

    VfsCapabilities capabilities() const override
    {
        return VfsCapability::Read | VfsCapability::ReportsSpace;
    }

    Result<FileEntryList> list(const VfsUri&, const CancelToken&) override { return FileEntryList {}; }

    Result<FileEntry> stat(const VfsUri& target) override
    {
        FileEntry entry;
        entry.uri = target;
        entry.name = target.fileName();
        entry.isDir = true;
        return entry;
    }

    Result<SpaceInfo> space(const VfsUri&) override
    {
        if (fail)
            return VfsError::make(VfsError::NotSupported, QStringLiteral("no idea"));
        SpaceInfo info;
        info.totalBytes = totalBytes;
        info.freeBytes = freeBytes;
        return info;
    }

    qint64 totalBytes = 1000;
    qint64 freeBytes = 250;
    bool fail = false;
};

} // namespace

class TestMountListModel : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void obeysTheModelContract();
    void reportsCapacityWhenTheBackendKnowsIt();
    void saysNothingWhenTheBackendCannotAnswer();
    void saysNothingWhenTheBackendRefuses();
    void aFullDriveReadsAsFull();
    void unmountingDropsItsFigures();

private:
    /// Adds a mount and waits for its space answer to arrive, if one is coming.
    void mountSized(const QString& id, std::shared_ptr<SizedFileSystem> fs);

    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<MountListModel> m_model;
};

void TestMountListModel::init()
{
    m_vfs = std::make_unique<VfsManager>();
    m_tasks = std::make_unique<TaskManager>();
    m_model = std::make_unique<MountListModel>(m_vfs.get(), m_tasks.get());
    // The periodic refresh would keep submitting work behind the assertions.
    m_model->setRefreshInterval(0);
}

void TestMountListModel::cleanup()
{
    m_model.reset();
    m_tasks.reset();
    m_vfs.reset();
}

void TestMountListModel::mountSized(const QString& id, std::shared_ptr<SizedFileSystem> fs)
{
    Mount mount;
    mount.id = id;
    mount.displayName = id;
    mount.root = VfsUri::fromString(QStringLiteral("sized://%1/").arg(id));
    mount.fileSystem = std::move(fs);
    m_vfs->addMount(mount);
}

void TestMountListModel::obeysTheModelContract()
{
    QAbstractItemModelTester tester(m_model.get(), QAbstractItemModelTester::FailureReportingMode::QtTest);
    mountSized(QStringLiteral("a"), std::make_shared<SizedFileSystem>());
    QCOMPARE(m_model->rowCount(), 1);
}

void TestMountListModel::reportsCapacityWhenTheBackendKnowsIt()
{
    auto fs = std::make_shared<SizedFileSystem>();
    fs->totalBytes = 1000;
    fs->freeBytes = 250;
    mountSized(QStringLiteral("a"), fs);

    const QModelIndex index = m_model->index(0, 0);
    QVERIFY(waitFor([&] { return index.data(MountListModel::HasSpaceRole).toBool(); }));

    // 750 of 1000 used.
    QCOMPARE(index.data(MountListModel::UsedFractionRole).toDouble(), 0.75);
    QVERIFY(!index.data(MountListModel::TotalTextRole).toString().isEmpty());
    QVERIFY(!index.data(MountListModel::FreeTextRole).toString().isEmpty());
}

void TestMountListModel::saysNothingWhenTheBackendCannotAnswer()
{
    // A plain memory drive has no capacity, and neither does a bucket. The
    // row must stay a name rather than gaining a bar showing an invented
    // number -- a chart is read as a fact.
    Mount mount;
    mount.id = QStringLiteral("scratch");
    mount.displayName = QStringLiteral("Scratch");
    mount.root = VfsUri::fromString(QStringLiteral("mem://scratch/"));
    mount.fileSystem = std::make_shared<MemoryFileSystem>();
    m_vfs->addMount(mount);

    const QModelIndex index = m_model->index(0, 0);
    QCOMPARE(index.data(MountListModel::HasSpaceRole).toBool(), false);
    QCOMPARE(index.data(MountListModel::TotalTextRole).toString(), QString());
    QCOMPARE(index.data(MountListModel::UsedFractionRole).toDouble(), 0.0);

    // And nothing was submitted for it: a backend that does not advertise the
    // capability is not asked at all.
    m_model->refreshSpace();
    QCOMPARE(index.data(MountListModel::HasSpaceRole).toBool(), false);
}

void TestMountListModel::saysNothingWhenTheBackendRefuses()
{
    auto fs = std::make_shared<SizedFileSystem>();
    fs->fail = true; // advertises the capability, then cannot answer
    mountSized(QStringLiteral("a"), fs);

    QSignalSpy changed(m_model.get(), &QAbstractItemModel::dataChanged);
    m_model->refreshSpace();
    QVERIFY(waitFor([this] { return m_tasks->activeCount() == 0; }));

    const QModelIndex index = m_model->index(0, 0);
    QCOMPARE(index.data(MountListModel::HasSpaceRole).toBool(), false);
}

void TestMountListModel::aFullDriveReadsAsFull()
{
    auto fs = std::make_shared<SizedFileSystem>();
    fs->totalBytes = 500;
    fs->freeBytes = 0;
    mountSized(QStringLiteral("a"), fs);

    const QModelIndex index = m_model->index(0, 0);
    QVERIFY(waitFor([&] { return index.data(MountListModel::HasSpaceRole).toBool(); }));
    QCOMPARE(index.data(MountListModel::UsedFractionRole).toDouble(), 1.0);
}

void TestMountListModel::unmountingDropsItsFigures()
{
    auto fs = std::make_shared<SizedFileSystem>();
    mountSized(QStringLiteral("a"), fs);

    QVERIFY(waitFor([this] { return m_model->index(0, 0).data(MountListModel::HasSpaceRole).toBool(); }));

    m_vfs->removeMount(QStringLiteral("a"));
    QCOMPARE(m_model->rowCount(), 0);

    // Mounted again under the same id, it must not inherit the old bar before
    // its own answer arrives.
    auto replacement = std::make_shared<SizedFileSystem>();
    replacement->fail = true;
    mountSized(QStringLiteral("a"), replacement);

    QCOMPARE(m_model->index(0, 0).data(MountListModel::HasSpaceRole).toBool(), false);
}

MOLE_TEST_MAIN(TestMountListModel)
#include "tst_MountListModel.moc"
