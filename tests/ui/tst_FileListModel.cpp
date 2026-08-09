#include "support/MoleTestMain.h"
#include "ui/models/FileListModel.h"

#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QSignalSpy>

using namespace mole;

namespace {

FileEntry makeEntry(const QString& name, bool isDir = false, qint64 size = 0, const QDateTime& modified = {})
{
    FileEntry entry;
    entry.name = name;
    entry.uri = VfsUri::fromString(QStringLiteral("file:///data")).child(name);
    entry.isDir = isDir;
    entry.isHidden = name.startsWith(QLatin1Char('.'));
    entry.size = size;
    entry.modified = modified.isValid() ? modified : QDateTime::fromSecsSinceEpoch(1700000000);
    return entry;
}

QStringList namesOf(const FileListModel& model)
{
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row)
        out.append(model.data(model.index(row, 0), FileListModel::NameRole).toString());
    return out;
}

} // namespace

class TestFileListModel : public QObject
{
    Q_OBJECT

private slots:
    void obeysTheModelContract();
    void hidesDotFilesByDefault();
    void showHiddenRevealsThem();
    void directoriesSortFirst();
    void directoriesStaySortedFirstWhenDescending();
    void sortsNamesNaturally();
    void sortsBySizeAndDate();
    void appendKeepsEarlierResults();
    void streamingManyResultsStaysCheap();
    void streamingScatteredResultsStaysCheap();
    void streamingInsertsRowsInsteadOfResetting();
    void clearEmptiesTheModel();
    void exposesRolesQmlNeeds();
    void uriAndKindLookupsAreBoundsChecked();
    void totalSizeIgnoresDirectories();
    void formatsSizes_data();
    void formatsSizes();

    void selectionTracksFilesNotRows();
    void selectionSurvivesResorting();
    void selectionDropsFilesThatDisappeared();
    void selectAllAndInvert();
    void targetsFallBackToTheCursor();
    void selectionIsBoundsChecked();

    void filterNarrowsTheListing();
    void filterIsCaseInsensitiveIncludingNonAscii();
    void filterCombinesWithHiddenFiles();
    void filterReportsHowMuchItHid();
    void clearingTheFilterRestoresEverything();
};

void TestFileListModel::obeysTheModelContract()
{
    FileListModel model;
    // Catches the classic beginInsertRows/endResetModel mistakes that only show
    // up later as a corrupted view.
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);

    model.setEntries({ makeEntry(QStringLiteral("a")), makeEntry(QStringLiteral("b")) });
    model.appendEntries({ makeEntry(QStringLiteral("c")) });
    model.setShowHidden(true);
    model.setSortKey(FileListModel::SortKey::Size);
    model.clear();
    QCOMPARE(model.rowCount(), 0);
}

void TestFileListModel::hidesDotFilesByDefault()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("visible")), makeEntry(QStringLiteral(".hidden")) });

    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(namesOf(model), QStringList({ QStringLiteral("visible") }));
}

void TestFileListModel::showHiddenRevealsThem()
{
    FileListModel model;
    QSignalSpy countSpy(&model, &FileListModel::countChanged);
    model.setEntries({ makeEntry(QStringLiteral("visible")), makeEntry(QStringLiteral(".hidden")) });

    model.setShowHidden(true);
    QCOMPARE(model.rowCount(), 2);
    QVERIFY(countSpy.count() >= 2);

    model.setShowHidden(false);
    QCOMPARE(model.rowCount(), 1);
}

void TestFileListModel::directoriesSortFirst()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("aaa-file")), makeEntry(QStringLiteral("zzz-dir"), true),
        makeEntry(QStringLiteral("bbb-file")) });

    QCOMPARE(namesOf(model),
        QStringList({ QStringLiteral("zzz-dir"), QStringLiteral("aaa-file"), QStringLiteral("bbb-file") }));
}

void TestFileListModel::directoriesStaySortedFirstWhenDescending()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("file")), makeEntry(QStringLiteral("dir"), true) });
    model.setSortDescending(true);

    // Reversing the order must not scatter folders through the listing.
    QCOMPARE(namesOf(model).first(), QStringLiteral("dir"));
}

void TestFileListModel::sortsNamesNaturally()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("file10")), makeEntry(QStringLiteral("file9")),
        makeEntry(QStringLiteral("file1")) });

    // "file9" before "file10" is what a person expects; strcmp disagrees.
    QCOMPARE(namesOf(model),
        QStringList({ QStringLiteral("file1"), QStringLiteral("file9"), QStringLiteral("file10") }));
}

void TestFileListModel::sortsBySizeAndDate()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("big"), false, 5000, QDateTime::fromSecsSinceEpoch(1000)),
        makeEntry(QStringLiteral("small"), false, 10, QDateTime::fromSecsSinceEpoch(9000)) });

    model.setSortKey(FileListModel::SortKey::Size);
    QCOMPARE(namesOf(model).first(), QStringLiteral("small"));

    model.setSortKey(FileListModel::SortKey::Modified);
    QCOMPARE(namesOf(model).first(), QStringLiteral("big"));

    model.setSortDescending(true);
    QCOMPARE(namesOf(model).first(), QStringLiteral("small"));
}

void TestFileListModel::appendKeepsEarlierResults()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("b")) });
    model.appendEntries({ makeEntry(QStringLiteral("a")) });

    // Streaming search results must accumulate, and stay sorted as they land.
    QCOMPARE(namesOf(model), QStringList({ QStringLiteral("a"), QStringLiteral("b") }));

    model.appendEntries({});
    QCOMPARE(model.rowCount(), 2);
}

void TestFileListModel::streamingManyResultsStaysCheap()
{
    // What a search over a large tree does: batch after batch into a model the
    // interface is watching. Re-sorting everything found so far on every batch is
    // quadratic, and it runs on the thread that draws the window -- which is how a
    // long search made the whole interface stop responding while a longer analysis,
    // which never touches a visible model, stayed smooth.
    FileListModel model;

    FileEntryList batch;
    const int batchSize = 200;
    const int batches = 200; // 40 000 results, an ordinary answer on a big disk

    QElapsedTimer clock;
    clock.start();
    for (int b = 0; b < batches; ++b) {
        batch.clear();
        for (int i = 0; i < batchSize; ++i)
            batch.append(makeEntry(QStringLiteral("result-%1-%2.txt").arg(b).arg(i), false, 10));
        model.appendEntries(batch);
    }
    const qint64 elapsed = clock.elapsed();

    QCOMPARE(model.rowCount(), batchSize * batches);
    // Generous, because this is a debug build on whatever machine happens to run
    // it. Quadratic behaviour does not come close to fitting inside it.
    QVERIFY2(elapsed < 3000,
        qPrintable(QStringLiteral("streaming %1 results in %2 batches took %3 ms; it has to stay near "
                                  "linear or the window stops answering during a search")
                       .arg(batchSize * batches)
                       .arg(batches)
                       .arg(elapsed)));

    // And the order is still right, which is the thing the sorting was for.
    QVERIFY(
        model.data(model.index(0, 0), FileListModel::UriRole).toString().contains(QStringLiteral("-0-0")));
}

/// The case above is kind to the model without meaning to be: every name in a
/// batch shares a prefix, so the batch lands as one stretch and one insertion.
/// A real search sorts by the base name while walking a tree, so a batch is two
/// hundred names scattered the length of the alphabet -- two hundred separate
/// insertions into a list of forty thousand, which is the shape that turns
/// "insert rather than reset" back into quadratic work.
void TestFileListModel::streamingScatteredResultsStaysCheap()
{
    FileListModel model;

    const int batchSize = 200;
    const int batches = 200;

    FileEntryList batch;
    QElapsedTimer clock;
    clock.start();
    for (int b = 0; b < batches; ++b) {
        batch.clear();
        for (int i = 0; i < batchSize; ++i) {
            // Interleaved on purpose: batch b holds every (batches)th name, so
            // each one lands between two rows that are already there.
            batch.append(
                makeEntry(QStringLiteral("file-%1.txt").arg(i * batches + b, 8, 10, QLatin1Char('0'))));
        }
        model.appendEntries(batch);
    }
    const qint64 elapsed = clock.elapsed();

    QCOMPARE(model.rowCount(), batchSize * batches);
    QVERIFY2(elapsed < 3000,
        qPrintable(QStringLiteral("streaming %1 scattered results in %2 batches took %3 ms; keeping the "
                                  "view's place must not cost what the re-sort used to")
                       .arg(batchSize * batches)
                       .arg(batches)
                       .arg(elapsed)));

    const QStringList names = namesOf(model);
    QCOMPARE(names.first(), QStringLiteral("file-00000000.txt"));
    QCOMPARE(names.at(1), QStringLiteral("file-00000001.txt"));
}

/// The point of the whole thing. A reset tells the view "everything you knew is
/// gone", and it answers by dropping the scroll position and the row under the
/// cursor -- so results arriving while somebody reads them pull the page away.
void TestFileListModel::streamingInsertsRowsInsteadOfResetting()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("b.txt")), makeEntry(QStringLiteral("d.txt")) });

    QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
    QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);

    // One before, one between, one after: three separate stretches, and the
    // rows already there must keep their identity through all of it.
    model.appendEntries({ makeEntry(QStringLiteral("a.txt")), makeEntry(QStringLiteral("c.txt")),
        makeEntry(QStringLiteral("e.txt")) });

    QCOMPARE(reset.count(), 0);
    QCOMPARE(inserted.count(), 3);
    QCOMPARE(namesOf(model),
        QStringList({ QStringLiteral("a.txt"), QStringLiteral("b.txt"), QStringLiteral("c.txt"),
            QStringLiteral("d.txt"), QStringLiteral("e.txt") }));

    // Each stretch named where it actually went, or the view moves the wrong
    // rows out of the way.
    QCOMPARE(inserted.at(0).at(1).toInt(), 0);
    QCOMPARE(inserted.at(1).at(1).toInt(), 2);
    QCOMPARE(inserted.at(2).at(1).toInt(), 4);

    // A batch that lands together is one insertion, not one per row.
    QSignalSpy more(&model, &QAbstractItemModel::rowsInserted);
    model.appendEntries({ makeEntry(QStringLiteral("f.txt")), makeEntry(QStringLiteral("g.txt")) });
    QCOMPARE(more.count(), 1);
    QCOMPARE(more.at(0).at(1).toInt(), 5);
    QCOMPARE(more.at(0).at(2).toInt(), 6);
}

void TestFileListModel::clearEmptiesTheModel()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("a")) });
    model.clear();
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(model.uriAt(0).isEmpty());
}

void TestFileListModel::exposesRolesQmlNeeds()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("report.pdf"), false, 2048) });

    const QHash<int, QByteArray> roles = model.roleNames();
    for (const char* name :
        { "name", "uri", "parentUri", "isDir", "sizeText", "modifiedText", "iconText", "suffix" }) {
        QVERIFY2(roles.values().contains(QByteArray(name)),
            qPrintable(QStringLiteral("role '%1' is missing").arg(QLatin1String(name))));
    }

    const QModelIndex index = model.index(0, 0);
    QCOMPARE(index.data(FileListModel::UriRole).toString(), QStringLiteral("file:///data/report.pdf"));
    QCOMPARE(index.data(FileListModel::ParentUriRole).toString(), QStringLiteral("file:///data"));
    QCOMPARE(index.data(FileListModel::SuffixRole).toString(), QStringLiteral("pdf"));
    QCOMPARE(index.data(FileListModel::SizeTextRole).toString(), QStringLiteral("2.0 kB"));
    QVERIFY(!index.data(FileListModel::IconTextRole).toString().isEmpty());
}

void TestFileListModel::uriAndKindLookupsAreBoundsChecked()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("dir"), true) });

    QCOMPARE(model.uriAt(0), QStringLiteral("file:///data/dir"));
    QVERIFY(model.isDirAt(0));

    // QML calls these with whatever index it has; they must never crash.
    QVERIFY(model.uriAt(-1).isEmpty());
    QVERIFY(model.uriAt(99).isEmpty());
    QVERIFY(!model.isDirAt(-1));
    QVERIFY(!model.isDirAt(99));
}

void TestFileListModel::totalSizeIgnoresDirectories()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("a"), false, 100), makeEntry(QStringLiteral("b"), false, 250),
        makeEntry(QStringLiteral("d"), true, 999) });

    QCOMPARE(model.totalSize(), 350);
}

void TestFileListModel::formatsSizes_data()
{
    QTest::addColumn<qint64>("bytes");
    QTest::addColumn<QString>("expected");

    QTest::newRow("zero") << qint64(0) << "0 B";
    QTest::newRow("bytes") << qint64(512) << "512 B";
    QTest::newRow("just under kB") << qint64(1023) << "1023 B";
    QTest::newRow("one kB") << qint64(1024) << "1.0 kB";
    QTest::newRow("large kB") << qint64(20480) << "20 kB";
    QTest::newRow("MB") << qint64(1024 * 1024) << "1.0 MB";
    QTest::newRow("GB") << qint64(3LL * 1024 * 1024 * 1024) << "3.0 GB";
    QTest::newRow("TB") << qint64(2LL * 1024 * 1024 * 1024 * 1024) << "2.0 TB";
    QTest::newRow("negative") << qint64(-1) << "";
}

void TestFileListModel::formatsSizes()
{
    QFETCH(qint64, bytes);
    QFETCH(QString, expected);
    QCOMPARE(FileListModel::formatSize(bytes), expected);
}

void TestFileListModel::selectionTracksFilesNotRows()
{
    FileListModel model;
    QSignalSpy changed(&model, &FileListModel::selectionChanged);
    model.setEntries({ makeEntry(QStringLiteral("a")), makeEntry(QStringLiteral("b")) });

    model.setSelected(0, true);
    QCOMPARE(model.selectionCount(), 1);
    QVERIFY(model.isSelected(0));
    QVERIFY(!model.isSelected(1));
    QCOMPARE(model.selectedUris(), QStringList({ QStringLiteral("file:///data/a") }));
    QVERIFY(changed.count() > 0);

    model.toggleSelected(0);
    QCOMPARE(model.selectionCount(), 0);
}

void TestFileListModel::selectionSurvivesResorting()
{
    FileListModel model;
    model.setEntries(
        { makeEntry(QStringLiteral("a"), false, 10), makeEntry(QStringLiteral("b"), false, 5000) });

    model.setSelected(0, true); // "a"
    model.setSortKey(FileListModel::SortKey::Size);

    // Row 0 is now "a" still (smallest first), but the real test is that the
    // selection followed the file rather than staying on a row number.
    QCOMPARE(model.selectedUris(), QStringList({ QStringLiteral("file:///data/a") }));

    model.setSortDescending(true);
    QCOMPARE(model.selectedUris(), QStringList({ QStringLiteral("file:///data/a") }));
    QVERIFY2(model.isSelected(1), "after reversing, the selected file moved to the other row");
}

void TestFileListModel::selectionDropsFilesThatDisappeared()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("stays")), makeEntry(QStringLiteral("goes")) });
    model.selectAll();
    QCOMPARE(model.selectionCount(), 2);

    // A refresh after someone else deleted a file must not leave the selection
    // pointing at something that no longer exists.
    model.setEntries({ makeEntry(QStringLiteral("stays")) });
    QCOMPARE(model.selectionCount(), 1);
    QCOMPARE(model.selectedUris(), QStringList({ QStringLiteral("file:///data/stays") }));
}

void TestFileListModel::selectAllAndInvert()
{
    FileListModel model;
    model.setEntries(
        { makeEntry(QStringLiteral("a")), makeEntry(QStringLiteral("b")), makeEntry(QStringLiteral("c")) });

    model.selectAll();
    QCOMPARE(model.selectionCount(), 3);

    model.invertSelection();
    QCOMPARE(model.selectionCount(), 0);

    model.setSelected(1, true);
    model.invertSelection();
    QCOMPARE(model.selectionCount(), 2);
    QVERIFY(!model.isSelected(1));

    model.clearSelection();
    QCOMPARE(model.selectionCount(), 0);
}

void TestFileListModel::targetsFallBackToTheCursor()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("a")), makeEntry(QStringLiteral("b")) });

    // Nothing ticked: act on whatever the cursor is on, like every commander.
    QList<VfsUri> targets = model.targets(1);
    QCOMPARE(targets.size(), 1);
    QCOMPARE(targets.first().fileName(), QStringLiteral("b"));

    // Once something is ticked, the cursor stops mattering.
    model.setSelected(0, true);
    targets = model.targets(1);
    QCOMPARE(targets.size(), 1);
    QCOMPARE(targets.first().fileName(), QStringLiteral("a"));

    QVERIFY(model.targets(-1).size() == 1);
    model.clearSelection();
    QVERIFY(model.targets(-1).isEmpty());
}

void TestFileListModel::selectionIsBoundsChecked()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("a")) });

    // QML hands over whatever index it has.
    model.setSelected(-1, true);
    model.setSelected(99, true);
    model.toggleSelected(42);
    QCOMPARE(model.selectionCount(), 0);
    QVERIFY(!model.isSelected(-1));
    QVERIFY(model.nameAt(99).isEmpty());
    QCOMPARE(model.rowOfUri(QStringLiteral("file:///data/a")), 0);
    QCOMPARE(model.rowOfUri(QStringLiteral("file:///nope")), -1);
}

void TestFileListModel::filterNarrowsTheListing()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("annual-report.pdf")), makeEntry(QStringLiteral("notes.txt")),
        makeEntry(QStringLiteral("reports"), true) });

    model.setFilterText(QStringLiteral("report"));

    // Substring, not prefix: people type the bit they remember.
    QCOMPARE(namesOf(model), QStringList({ QStringLiteral("reports"), QStringLiteral("annual-report.pdf") }));
    QCOMPARE(model.rowCount(), 2);
}

void TestFileListModel::filterIsCaseInsensitiveIncludingNonAscii()
{
    FileListModel model;
    model.setEntries(
        { makeEntry(QStringLiteral("ŁÓDŹ-raport.txt")), makeEntry(QStringLiteral("other.txt")) });

    // Folded with Qt, not with an ASCII-only rule, so this matches.
    model.setFilterText(QStringLiteral("łódź"));
    QCOMPARE(model.rowCount(), 1);

    model.setFilterText(QStringLiteral("RAPORT"));
    QCOMPARE(model.rowCount(), 1);
}

void TestFileListModel::filterCombinesWithHiddenFiles()
{
    FileListModel model;
    model.setEntries(
        { makeEntry(QStringLiteral(".hidden-report")), makeEntry(QStringLiteral("visible-report")) });

    model.setFilterText(QStringLiteral("report"));
    // Filtering must not quietly reveal what the hidden-files rule excluded.
    QCOMPARE(namesOf(model), QStringList({ QStringLiteral("visible-report") }));

    model.setShowHidden(true);
    QCOMPARE(model.rowCount(), 2);
}

void TestFileListModel::filterReportsHowMuchItHid()
{
    FileListModel model;
    model.setEntries({ makeEntry(QStringLiteral("a-match")), makeEntry(QStringLiteral("b")),
        makeEntry(QStringLiteral("c")) });

    model.setFilterText(QStringLiteral("match"));
    // "1 of 3" in the status line needs both numbers.
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.totalCount(), 3);
}

void TestFileListModel::clearingTheFilterRestoresEverything()
{
    FileListModel model;
    QSignalSpy filterSpy(&model, &FileListModel::filterChanged);
    model.setEntries({ makeEntry(QStringLiteral("a")), makeEntry(QStringLiteral("b")) });

    model.setFilterText(QStringLiteral("a"));
    QCOMPARE(model.rowCount(), 1);

    model.setFilterText(QString());
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(filterSpy.count(), 2);

    // A filter that matches nothing is empty, not an error.
    model.setFilterText(QStringLiteral("zzz"));
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(model.totalCount(), 2);
}

MOLE_TEST_MAIN(TestFileListModel)
#include "tst_FileListModel.moc"
