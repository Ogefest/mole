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
/// mode. Both of those are choices about exactly this moment: a kill lands
/// between two transactions or inside one, and either way what is on disk has to
/// be a database that opens. If it does not, every drive the user has ever
/// scanned has to be walked again — which on the sizes this program exists for
/// is hours.
void TestKilledOutright::anIndexKilledMidWriteOpensAgainAndAnswers()
{
    if (Victim::isThisProcess()) {
        IndexDatabase database(Victim::instruction());
        if (!database.open().ok())
            return;
        const Result<qint64> volume
            = database.upsertVolume(VfsUri::fromLocalPath(QStringLiteral("/tmp")), QStringLiteral("scratch"));
        if (!volume.ok())
            return;

        for (int batch = 0; batch < 100000; ++batch) {
            QList<IndexedFile> files;
            for (int i = 0; i < 500; ++i) {
                IndexedFile file;
                file.name = QStringLiteral("file-%1-%2.txt").arg(batch).arg(i);
                file.path = QStringLiteral("/tmp/%1").arg(file.name);
                file.parentPath = QStringLiteral("/tmp");
                file.extension = QStringLiteral("txt");
                file.size = 1234;
                files.append(file);
            }
            if (!database.insertBatch(volume.value(), files).ok())
                return;
        }
        return;
    }

    TempTree tree;
    const QString path = tree.absolute(QStringLiteral("index.sqlite"));

    Victim victim(QStringLiteral("anIndexKilledMidWriteOpensAgainAndAnswers"), path);
    QVERIFY(victim.started());

    // Killed once rows are actually going in, so the kill lands during a write
    // rather than before the schema exists.
    const bool writing = victim.waitUntil([&path] { return QFileInfo(path).size() > 32 * 1024; });
    victim.kill();

    QVERIFY2(writing,
        qPrintable(QStringLiteral("the victim never wrote an index. It said: %1").arg(victim.transcript())));

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

    IndexSearchQuery query;
    query.text = QStringLiteral("file-0-");
    const Result<QList<IndexSearchHit>> hits = reopened.search(query);
    QVERIFY2(hits.ok(), qPrintable(hits.error().message));
    QVERIFY2(!hits.value().isEmpty(),
        "the index opened and knows nothing, which is an index that has to be built again");
}

MOLE_TEST_MAIN(TestKilledOutright)

#include "tst_KilledOutright.moc"
