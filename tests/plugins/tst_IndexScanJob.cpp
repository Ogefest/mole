#include "plugins/builtin/IndexScanJob.h"
#include "sdk/IMetadataReader.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/automation/ScheduleStore.h"
#include "core/events/EventBus.h"
#include "core/index/IndexDatabase.h"
#include "core/index/ScanTask.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

namespace {

/// One fact about a photograph, so a test can ask a question the index can
/// only answer if the scan really read the files.
class CameraReader final : public IMetadataReader
{
public:
    QString id() const override { return QStringLiteral("test.camera"); }
    bool canRead(const FileEntry& entry) const override { return entry.uri.suffix() == QLatin1String("jpg"); }
    QList<FileFact> read(const FileEntry& entry, QByteArrayView head, const PluginServices& services,
        const CancelToken& cancel) const override
    {
        Q_UNUSED(entry);
        Q_UNUSED(head);
        Q_UNUSED(services);
        Q_UNUSED(cancel);
        return { FileFact {
            QStringLiteral("Camera"), QStringLiteral("X100V"), QStringLiteral("image.camera") } };
    }
};

class OneReader final : public IMetadataLookup
{
public:
    QList<IMetadataReader*> readersFor(const FileEntry& entry) const override
    {
        return m_reader.canRead(entry) ? QList<IMetadataReader*> { &m_reader } : QList<IMetadataReader*> {};
    }

private:
    mutable CameraReader m_reader;
};

/// A drive that mounts a `.bag` file, standing in for the archive backend so
/// the container half of a scan can be tested in a build without one. What
/// is inside is fixed, because what is being tested is whether the scan
/// asked at all.
class BagFactory final : public IFileSystemFactory
{
public:
    QString scheme() const override { return QStringLiteral("bag"); }
    QString displayName() const override { return QStringLiteral("Bag"); }
    QStringList mountableFileSuffixes() const override { return { QStringLiteral("bag") }; }

    QVariantMap configForFile(const QString& localPath) const override
    {
        return { { QStringLiteral("path"), localPath } };
    }
    VfsUri rootUriForFile(const QString& localPath) const override
    {
        return VfsUri(QStringLiteral("bag"), localPath, QStringLiteral("/"));
    }
    FileSystemPtr create(const QVariantMap& config, QString* errorOut) override
    {
        Q_UNUSED(errorOut);
        Q_UNUSED(config);
        auto inside = std::make_shared<MemoryFileSystem>();
        inside->addFile(QStringLiteral("/packed.txt"), QByteArray("in the bag"));
        return inside;
    }
};

}

/// The nightly re-index, and whether it repeats the scan that created it.
///
/// A rule used to carry nothing but the folder and the incremental flag, so
/// every subtree the scan re-walked was rewritten with rows that said nothing
/// about the files -- the feature got worse the longer it ran. See MOLE-226.
class TestIndexScanJob : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aRuleAskingForMetadataAndArchivesGetsThem();
    void aRuleAskingForNeitherWritesWhatItAlwaysWrote();
    void aRuleCarriesTheOptionsItWasMadeWith();
    void aRuleFromBeforeAnOptionExistedGetsWhatTheDialogWouldHaveAsked();
    void anArchiveInAnUnchangedFolderKeepsItsMembersAcrossARescan();
    void aSecondScanOfAVolumeAlreadyBeingScannedIsRefused();

private:
    /// Runs the job for `rule` and returns once its scan has finished.
    bool run(const ScheduleRule& rule);
    ScheduleRule ruleFor(bool metadata, bool archives) const;
    /// How many rows the index has for `name`, which is how both halves of a
    /// scan are asked about.
    int rowsNamed(const QString& name) const;
    int photographsFrom(const QString& camera) const;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<TaskManager> m_tasks;
    std::unique_ptr<EventBus> m_events;
    std::unique_ptr<IndexDatabase> m_index;
    std::unique_ptr<OneReader> m_metadata;
    PluginServices m_services;
};

void TestIndexScanJob::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    QVERIFY(m_tree->writeFile(QStringLiteral("photos/a.jpg")));
    QVERIFY(m_tree->writeFile(QStringLiteral("archives/holder.bag")));

    m_vfs = std::make_unique<VfsManager>();
    m_vfs->registerFactory(std::make_unique<LocalFileSystemFactory>());
    m_vfs->registerFactory(std::make_unique<BagFactory>());
    Mount mount;
    mount.displayName = QStringLiteral("tree");
    mount.root = m_tree->rootUri();
    mount.fileSystem = std::make_shared<LocalFileSystem>();
    QVERIFY(!m_vfs->addMount(mount).isEmpty());

    m_tasks = std::make_unique<TaskManager>();
    m_events = std::make_unique<EventBus>();
    m_index = std::make_unique<IndexDatabase>(QDir(m_dir->path()).filePath(QStringLiteral("index.sqlite")));
    QVERIFY(m_index->open().ok());
    m_metadata = std::make_unique<OneReader>();

    m_services = PluginServices { m_vfs.get(), m_tasks.get(), m_index.get(), m_events.get() };
    m_services.metadata = m_metadata.get();
}

void TestIndexScanJob::cleanup()
{
    m_tasks.reset();
    m_index.reset();
    m_metadata.reset();
    m_events.reset();
    m_vfs.reset();
    m_tree.reset();
    m_dir.reset();
}

ScheduleRule TestIndexScanJob::ruleFor(bool metadata, bool archives) const
{
    ScheduleRule rule;
    rule.id = QStringLiteral("nightly");
    rule.jobKind = IndexScanJob::kind();
    rule.label = QStringLiteral("Re-index the tree");
    rule.intervalSeconds = 24 * 3600;
    rule.parameters = { { IndexScanJob::rootUriParameter(), m_tree->rootUri().toString() },
        { IndexScanJob::incrementalParameter(), true }, { IndexScanJob::metadataParameter(), metadata },
        { IndexScanJob::archivesParameter(), archives } };
    return rule;
}

bool TestIndexScanJob::run(const ScheduleRule& rule)
{
    IndexScanJob job(m_services);
    bool finished = false;
    bool succeeded = false;
    if (job.start(rule,
               [&](bool ok, QString) {
                   succeeded = ok;
                   finished = true;
               })
            .what
        != StartOutcome::What::Started) {
        return false;
    }
    // On the condition rather than on a clock: the job hands its scan to the
    // task manager and answers when that finishes.
    if (!waitFor([&] { return finished; }, 30000))
        return false;
    return succeeded;
}

int TestIndexScanJob::rowsNamed(const QString& name) const
{
    SearchQuery query;
    query.add(SearchPredicate::name(name));
    const Result<QList<IndexSearchHit>> hits = m_index->search(query);
    return hits.ok() ? int(hits.value().size()) : -1;
}

int TestIndexScanJob::photographsFrom(const QString& camera) const
{
    SearchQuery query;
    query.add(SearchPredicate::metadataIs(QStringLiteral("image.camera"), camera));
    const Result<QList<IndexSearchHit>> hits = m_index->search(query);
    return hits.ok() ? int(hits.value().size()) : -1;
}

/// The fault: a rule made from a scan that recorded metadata and archive
/// contents used to repeat it as a scan that recorded neither.
void TestIndexScanJob::aRuleAskingForMetadataAndArchivesGetsThem()
{
    QVERIFY(run(ruleFor(true, true)));

    QCOMPARE(photographsFrom(QStringLiteral("x100v")), 1);
    QCOMPARE(rowsNamed(QStringLiteral("packed.txt")), 1);
}

/// And the other half, so the fix is not "always read everything": a rule that
/// asks for neither still costs a walk and nothing more.
void TestIndexScanJob::aRuleAskingForNeitherWritesWhatItAlwaysWrote()
{
    QVERIFY(run(ruleFor(false, false)));

    QCOMPARE(photographsFrom(QStringLiteral("x100v")), 0);
    QCOMPARE(rowsNamed(QStringLiteral("packed.txt")), 0);
    // The files themselves are indexed either way.
    QCOMPARE(rowsNamed(QStringLiteral("a.jpg")), 1);
}

void TestIndexScanJob::aRuleCarriesTheOptionsItWasMadeWith()
{
    const ScanOptions asked = IndexScanJob::optionsFor(ruleFor(true, false));
    QVERIFY(asked.incremental);
    QVERIFY(asked.metadata);
    QVERIFY(!asked.archives);

    // What a rule that does not say gets is the next case: it used to be "a walk
    // and no more", asserted here, and that was the disagreement -- every other
    // caller opens with archives on.
}

/// A rule written before an option existed.
///
/// Every parameter is read with a default, and the defaults here disagreed with
/// every other caller: `archives` was off, and the index dialog and the search
/// form both open with it on. So a nightly rule made by a version that had no
/// archive parameter rebuilt the volume every night as a poorer scan than the
/// one that created it -- exactly the drift ADR-0057 was written against, one
/// version later. There is one set of defaults now.
void TestIndexScanJob::aRuleFromBeforeAnOptionExistedGetsWhatTheDialogWouldHaveAsked()
{
    ScheduleRule old;
    old.id = QStringLiteral("nightly");
    old.jobKind = IndexScanJob::kind();
    old.intervalSeconds = 24 * 3600;
    // Only what the earlier version wrote: the folder and the incremental flag.
    old.parameters = { { IndexScanJob::rootUriParameter(), m_tree->rootUri().toString() },
        { IndexScanJob::incrementalParameter(), true } };

    const ScanOptions options = IndexScanJob::optionsFor(old);

    QCOMPARE(options.archives, ScanOptions::dialogDefaults().archives);
    QCOMPARE(options.metadata, ScanOptions::dialogDefaults().metadata);
    QVERIFY2(options.incremental, "a nightly rule stays incremental whatever else it does not say");
}

/// The fault ADR-0056's machinery was built to stop, coming back through the
/// column the machinery works on.
///
/// A member's row used to carry the path *inside* the archive. Every prefix
/// question the index asks is asked on that column -- carrying an unchanged
/// folder forward copies the rows whose path is under it -- so a member's row
/// was not among them, and the subtree is skipped, so nothing re-walked it
/// either. A nightly re-index quietly dropped the contents of every archive in
/// a folder that had not changed, which is most of them.
void TestIndexScanJob::anArchiveInAnUnchangedFolderKeepsItsMembersAcrossARescan()
{
    // Settled, so the second scan has something it is allowed to trust.
    const QDateTime before = QDateTime::currentDateTime().addSecs(-3600);
    QVERIFY(setModifiedTime(m_tree->absolute(QStringLiteral("archives/holder.bag")), before));
    QVERIFY(setModifiedTime(m_tree->absolute(QStringLiteral("archives")), before));
    QVERIFY(setModifiedTime(m_tree->absolute(QStringLiteral("photos")), before));
    QVERIFY(setModifiedTime(m_tree->path(), before));

    QVERIFY(run(ruleFor(false, true)));
    QCOMPARE(rowsNamed(QStringLiteral("packed.txt")), 1);

    // Nothing has changed, so the second run carries everything rather than
    // walking it -- which is the path the member has to survive.
    QVERIFY(run(ruleFor(false, true)));

    QCOMPARE(rowsNamed(QStringLiteral("packed.txt")), 1);
}

void TestIndexScanJob::aSecondScanOfAVolumeAlreadyBeingScannedIsRefused()
{
    // One scan per volume at a time: two at once have the second one's
    // generation swap drop the first one's rows, so a rule firing while
    // somebody is rescanning by hand is not a slower answer but a wrong one.
    auto* first = new ScanTask(
        m_vfs->resolve(m_tree->rootUri()), m_tree->rootUri(), QStringLiteral("by hand"), m_index.get());
    m_tasks->submit(first);

    CapturedWarnings logged;
    IndexScanJob job(m_services);
    const StartOutcome outcome = job.start(ruleFor(false, false), [](bool, QString) {});

    QVERIFY(waitForTask(first, 30000));
    // Skipped and not Failed: nothing about the rule is wrong, and the streak the
    // tracking list ranks by must not count it. See MOLE-379.
    QCOMPARE(outcome.what, StartOutcome::What::Skipped);
    QVERIFY2(outcome.reason.contains(QStringLiteral("already being scanned")), qPrintable(outcome.reason));
    QVERIFY2(logged.contains(QStringLiteral("already being scanned")), qPrintable(logged.joined()));
}

MOLE_TEST_MAIN(TestIndexScanJob)
#include "tst_IndexScanJob.moc"
