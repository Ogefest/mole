#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "support/Victim.h"

#include "core/index/IndexDatabase.h"
#include "core/settings/Preferences.h"
#include "core/vfs/PartialWrite.h"
#include "core/vfs/backends/LocalFileSystem.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTest>

using namespace mole;
using namespace mole::test;

namespace {

QByteArray blockOf(int index)
{
    QByteArray block(256 * 1024, Qt::Uninitialized);
    for (int i = 0; i < block.size(); ++i)
        block[i] = static_cast<char>((i * 29 + index * 7) & 0xff);
    return block;
}

} // namespace

/// What a process killed outright leaves behind.
///
/// Every other failure gets a chance to tidy up: an error path runs, a
/// destructor runs, something deletes what it half wrote. `SIGKILL` gets none of
/// that, which makes it the only failure whose answer has to be built into what
/// was on disk before anything went wrong. These tests kill a real process,
/// because that is the one thing about this case that cannot be faked.
class TestKilledOutright : public QObject
{
    Q_OBJECT

private slots:
    void aCopyKilledMidWayKeepsTheOriginalAndLeavesAnObviousPartial();
    void aCopyKilledMidWayCanSimplyBeRunAgain();
    void preferencesKilledMidSaveAreTheOldOnesRatherThanHalfOfTheNew();
    void anIndexKilledMidWriteOpensAgainAndAnswers();
};

/// The one that loses a file that was already there.
///
/// A copy over an existing file used to open it with Truncate: the old contents
/// were gone before the first byte of the new ones arrived. A kill in between
/// left neither — not the file being replaced and not the file replacing it.
void TestKilledOutright::aCopyKilledMidWayKeepsTheOriginalAndLeavesAnObviousPartial()
{
    if (Victim::isThisProcess()) {
        // Writes until somebody stops it. Nothing here is asserted: this
        // process exists to be killed.
        LocalFileSystem fs;
        const VfsUri target = VfsUri::fromLocalPath(Victim::instruction());
        Result<std::unique_ptr<QIODevice>> opened = fs.openWrite(target, -1);
        if (!opened.ok())
            return;
        for (int block = 0; block < 4096; ++block) {
            if (opened.value()->write(blockOf(block)) <= 0)
                break;
        }
        return;
    }

    TempTree tree;
    const QString destination = tree.absolute(QStringLiteral("important.bin"));
    const QByteArray original = QByteArrayLiteral("the file that was already there");
    QVERIFY(tree.writeFile(QStringLiteral("important.bin"), original));

    Victim victim(QStringLiteral("aCopyKilledMidWayKeepsTheOriginalAndLeavesAnObviousPartial"), destination);
    QVERIFY2(victim.started(), "could not start a second copy of this test binary");

    const QString partial = partialWriteOf(VfsUri::fromLocalPath(destination)).toLocalPath();
    // Killed on the write having begun, not after a wait: until there are bytes
    // on disk there is nothing to interrupt.
    //
    // "Begun" means *either* name has moved, and deliberately so. Watching only
    // for the working name would make a copy that writes straight to the
    // destination look like a copy that never started -- which is untrue, and
    // hides the fault behind the least useful thing this could report.
    const qint64 was = original.size();
    const bool begun = victim.waitUntil([&partial, &destination, was] {
        return QFileInfo(partial).size() > 0 || QFileInfo(destination).size() != was;
    });
    victim.kill();

    QVERIFY2(begun,
        qPrintable(QStringLiteral("the write never started. The victim said: %1").arg(victim.transcript())));

    QFile file(destination);
    QVERIFY2(file.exists(), "the file being overwritten disappeared when the copy was interrupted");
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
    file.close();

    // And what the kill did leave is visibly not a file anybody asked for.
    QVERIFY2(QFileInfo::exists(partial), "there should be something to show for the bytes that were written");
    QVERIFY(isPartialWrite(QFileInfo(partial).fileName()));

    QFile::remove(partial);
}

/// And the wreckage does not get in the way of putting it right.
void TestKilledOutright::aCopyKilledMidWayCanSimplyBeRunAgain()
{
    if (Victim::isThisProcess()) {
        LocalFileSystem fs;
        const VfsUri target = VfsUri::fromLocalPath(Victim::instruction());
        Result<std::unique_ptr<QIODevice>> opened = fs.openWrite(target, -1);
        if (!opened.ok())
            return;
        for (int block = 0; block < 4096; ++block) {
            if (opened.value()->write(blockOf(block)) <= 0)
                break;
        }
        return;
    }

    TempTree tree;
    const QString destination = tree.absolute(QStringLiteral("report.bin"));

    Victim victim(QStringLiteral("aCopyKilledMidWayCanSimplyBeRunAgain"), destination);
    QVERIFY(victim.started());

    const QString partial = partialWriteOf(VfsUri::fromLocalPath(destination)).toLocalPath();
    const bool begun = victim.waitUntil(
        [&partial, &destination] { return QFileInfo(partial).size() > 0 || QFileInfo::exists(destination); });
    victim.kill();
    QVERIFY2(begun, qPrintable(victim.transcript()));

    // The same copy again, this time finishing.
    LocalFileSystem fs;
    const VfsUri target = VfsUri::fromLocalPath(destination);
    Result<std::unique_ptr<QIODevice>> again = fs.openWrite(target, -1);
    QVERIFY2(again.ok(), qPrintable(again.error().message));
    const QByteArray contents = QByteArrayLiteral("the whole thing, second time around");
    QCOMPARE(again.value()->write(contents), static_cast<qint64>(contents.size()));
    const Result<void> written = closeAndReport(*again.value());
    QVERIFY2(written.ok(), qPrintable(written.error().message));

    QFile file(destination);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), contents);
    file.close();

    QFile::remove(partial);
}

/// Configuration survives, or it was never configuration.
///
/// Preferences are written wholesale — the file is replaced every time anything
/// changes — so a kill part way through the write is a kill part way through the
/// only copy. Half a JSON document does not parse, and what does not parse is
/// every setting the user ever chose.
void TestKilledOutright::preferencesKilledMidSaveAreTheOldOnesRatherThanHalfOfTheNew()
{
    if (Victim::isThisProcess()) {
        // Saves over and over. One of them is interrupted, and which one does
        // not matter: what matters is that no moment in the middle of a save is
        // a moment at which the file is neither one thing nor the other.
        Preferences preferences(Victim::instruction());
        preferences.load();
        for (int round = 0; round < 100000; ++round) {
            preferences.setValue(QStringLiteral("theme"), QStringLiteral("dark-%1").arg(round));
            preferences.setValue(QStringLiteral("padding"), round);
            preferences.save();
        }
        return;
    }

    TempTree tree;
    const QString path = tree.absolute(QStringLiteral("preferences.json"));

    {
        Preferences settled(path);
        settled.setValue(QStringLiteral("theme"), QStringLiteral("light"));
        QVERIFY(settled.save());
    }

    Victim victim(QStringLiteral("preferencesKilledMidSaveAreTheOldOnesRatherThanHalfOfTheNew"), path);
    QVERIFY(victim.started());

    // Killed once the victim has written at least once, so the kill lands
    // somewhere in the middle of the stream of saves rather than before any.
    const bool saving = victim.waitUntil([&path] {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        return file.readAll().contains("dark-");
    });
    victim.kill();

    QVERIFY2(saving,
        qPrintable(QStringLiteral("the victim never saved anything. It said: %1").arg(victim.transcript())));

    QFile file(path);
    QVERIFY2(file.exists(), "the preferences file was removed by being written to");
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray contents = file.readAll();
    file.close();

    QJsonParseError problem {};
    const QJsonDocument document = QJsonDocument::fromJson(contents, &problem);
    QVERIFY2(problem.error == QJsonParseError::NoError,
        qPrintable(QStringLiteral("the file left behind is not JSON (%1), so every setting in it is lost: %2")
                       .arg(problem.errorString(), QString::fromUtf8(contents.left(200)))));
    QVERIFY2(document.isObject(), "the file left behind is not a settings document");

    // And it is a whole one -- some complete save, not a prefix of the next.
    Preferences reloaded(path);
    QVERIFY2(reloaded.load(), "what survived does not load");
    QVERIFY2(!reloaded.value(QStringLiteral("theme")).toString().isEmpty(),
        "the settings loaded, and they are empty, which is the same loss wearing a tidier face");
}

/// A scan interrupted is a scan to run again, not an index to rebuild.
///
/// The index is written in batches inside transactions, on a database in WAL
/// mode, and a scan's rows only become the volume's contents in the one
/// transaction that ends it. All three are choices about exactly this moment: a
/// kill lands between two transactions or inside one, and either way what is on
/// disk has to be a database that opens and still answers with the last scan
/// that finished. If it does not, every drive the user has ever scanned has to
/// be walked again — which on the sizes this program exists for is hours.
///
/// So the victim finishes one scan and is killed during the next. What it had
/// before the kill is what it must still have after it.
void TestKilledOutright::anIndexKilledMidWriteOpensAgainAndAnswers()
{
    const auto rowsNamed = [](const QString& prefix, int count) {
        QList<IndexedFile> files;
        files.reserve(count);
        for (int i = 0; i < count; ++i) {
            IndexedFile file;
            file.name = QStringLiteral("%1-%2.txt").arg(prefix).arg(i);
            file.path = QStringLiteral("/tmp/%1").arg(file.name);
            file.parentPath = QStringLiteral("/tmp");
            file.extension = QStringLiteral("txt");
            file.size = 1234;
            files.append(file);
        }
        return files;
    };
    // The database and its write-ahead log together: a batch lands in the log
    // and is moved into the database at a checkpoint, so either alone stops
    // growing at moments that have nothing to do with the scan.
    const auto indexBytes = [](const QString& path) {
        return QFileInfo(path).size() + QFileInfo(path + QStringLiteral("-wal")).size();
    };

    if (Victim::isThisProcess()) {
        IndexDatabase database(Victim::instruction());
        if (!database.open().ok())
            return;
        const Result<qint64> volume
            = database.upsertVolume(VfsUri::fromLocalPath(QStringLiteral("/tmp")), QStringLiteral("scratch"));
        if (!volume.ok())
            return;

        // One finished scan, which is what there is to lose.
        const Result<qint64> settled = database.beginScan(volume.value());
        if (!settled.ok())
            return;
        if (!database.insertBatch(volume.value(), settled.value(), rowsNamed(QStringLiteral("settled"), 500))
                 .ok())
            return;
        if (!database
                 .commitScan(volume.value(), settled.value(), QDateTime::currentDateTime(), ScanOptions {})
                 .ok())
            return;

        // Said with a file rather than a message, because the parent has to
        // know the first scan is committed before it starts watching for the
        // second, and a pipe from a process about to be killed is not a thing
        // to make that decision on.
        QFile marker(Victim::instruction() + QStringLiteral(".settled"));
        if (!marker.open(QIODevice::WriteOnly))
            return;
        marker.close();

        const Result<qint64> rescan = database.beginScan(volume.value());
        if (!rescan.ok())
            return;
        for (int batch = 0; batch < 100000; ++batch) {
            if (!database
                     .insertBatch(
                         volume.value(), rescan.value(), rowsNamed(QStringLiteral("file-%1").arg(batch), 500))
                     .ok())
                return;
        }
        return;
    }

    TempTree tree;
    const QString path = tree.absolute(QStringLiteral("index.sqlite"));
    const QString marker = path + QStringLiteral(".settled");

    Victim victim(QStringLiteral("anIndexKilledMidWriteOpensAgainAndAnswers"), path);
    QVERIFY(victim.started());

    // The kill has to land inside the second scan: after the first is
    // committed, and once the second is really putting rows on the disk.
    const bool committed = victim.waitUntil([&marker] { return QFileInfo::exists(marker); });
    const qint64 settledBytes = indexBytes(path);
    const bool rescanning
        = committed && victim.waitUntil([&] { return indexBytes(path) > settledBytes + 256 * 1024; });
    victim.kill();

    QVERIFY2(committed,
        qPrintable(QStringLiteral("the victim never finished a scan. It said: %1").arg(victim.transcript())));
    QVERIFY2(rescanning,
        qPrintable(QStringLiteral("the victim never started the rescan the kill has to land in. It said: %1")
                       .arg(victim.transcript())));

    IndexDatabase reopened(path);
    const Result<void> opened = reopened.open();
    QVERIFY2(opened.ok(),
        qPrintable(QStringLiteral("the index does not open after a kill, so every scan is lost: %1")
                       .arg(opened.error().message)));

    // Opening is not the same as being usable. A database that opens and then
    // refuses every question is the same loss with a later diagnosis.
    const Result<QList<IndexVolume>> volumes = reopened.volumes();
    QVERIFY2(volumes.ok(), qPrintable(volumes.error().message));
    QCOMPARE(volumes.value().size(), 1);

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("settled-")));
    const Result<QList<IndexSearchHit>> hits = reopened.search(query);
    QVERIFY2(hits.ok(), qPrintable(hits.error().message));
    QVERIFY2(!hits.value().isEmpty(),
        "the index opened and knows nothing, which is an index that has to be built again");
    QCOMPARE(reopened.fileCount().value(), 500);

    // And nothing from the scan that was killed: it was never committed, so it
    // was never the volume's contents, and a half-walked tree must not become
    // the answer merely because the process that was walking it died.
    SearchQuery gone;
    gone.add(SearchPredicate::name(QStringLiteral("file-0-")));
    QVERIFY2(reopened.search(gone).value().isEmpty(),
        "half of an interrupted rescan became the index, which is a search answering short and sure");
}

MOLE_TEST_MAIN(TestKilledOutright)

#include "tst_KilledOutright.moc"
