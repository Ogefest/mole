#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/DragSource.h"

#include <QMimeData>
#include <QSignalSpy>
#include <QUrl>

using namespace mole;
using namespace mole::test;

/// The drag source is the one place that hands a selection to the desktop, so
/// every test here replaces that final step with a recorder. Nothing is actually
/// dragged: `QDrag::exec()` wants a platform and a pointer, and this binary has
/// neither -- see ADR-0040 for why the seam is where it is.
class TestDragSource : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void localFilesBecomeOneUriList();
    void aDirectoryGoesOutAsItsOwnUrl();
    void theHookIsOfferedCopyAndNothingElse();
    void rowsThatAreNotOnDiskStartNothingAndSayWhy();
    void aMixedSelectionSendsWhatItCanAndSaysHowManyStayed();
    void anEmptySelectionStartsNothing();
    void aRefusedDragSaysSo();

private:
    DragSource* makeSource();

    QList<QUrl> m_urls;
    QStringList m_formats;
    Qt::DropActions m_actions = Qt::IgnoreAction;
    int m_handovers = 0;
    bool m_hookResult = true;
};

void TestDragSource::init()
{
    m_urls.clear();
    m_formats.clear();
    m_actions = Qt::IgnoreAction;
    m_handovers = 0;
    m_hookResult = true;
}

DragSource* TestDragSource::makeSource()
{
    auto* source = new DragSource(this);
    source->setStartHook([this](std::unique_ptr<QMimeData> mime, Qt::DropActions actions) {
        ++m_handovers;
        m_urls = mime->urls();
        m_formats = mime->formats();
        m_actions = actions;
        return m_hookResult;
    });
    return source;
}

void TestDragSource::localFilesBecomeOneUriList()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("first.txt")));
    QVERIFY(tree.writeFile(QStringLiteral("second.txt")));
    QVERIFY(tree.writeFile(QStringLiteral("third.txt")));

    DragSource* source = makeSource();
    QSignalSpy started(source, &DragSource::started);

    // The order is the order the rows were given: a receiver that lists what it
    // was handed shows the user's own selection back to them.
    source->start({
        VfsUri::fromLocalPath(tree.absolute(QStringLiteral("first.txt"))),
        VfsUri::fromLocalPath(tree.absolute(QStringLiteral("second.txt"))),
        VfsUri::fromLocalPath(tree.absolute(QStringLiteral("third.txt"))),
    });

    QCOMPARE(m_handovers, 1);
    QVERIFY(m_formats.contains(QStringLiteral("text/uri-list")));
    QCOMPARE(m_urls.size(), 3);
    QVERIFY(m_urls.at(0).isLocalFile());
    QCOMPARE(m_urls.at(0).toLocalFile(), tree.absolute(QStringLiteral("first.txt")));
    QCOMPARE(m_urls.at(1).toLocalFile(), tree.absolute(QStringLiteral("second.txt")));
    QCOMPARE(m_urls.at(2).toLocalFile(), tree.absolute(QStringLiteral("third.txt")));
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.first().first().toInt(), 3);
}

void TestDragSource::aDirectoryGoesOutAsItsOwnUrl()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.makeDirs(QStringLiteral("photos/2026")));
    QVERIFY(tree.writeFile(QStringLiteral("photos/2026/beach.jpg")));

    DragSource* source = makeSource();
    source->start({ VfsUri::fromLocalPath(tree.absolute(QStringLiteral("photos"))) });

    // One url for the folder, not one per file underneath it. Expanding it here
    // would hand the receiver a flat list of leaves.
    QCOMPARE(m_urls.size(), 1);
    QCOMPARE(m_urls.first().toLocalFile(), tree.absolute(QStringLiteral("photos")));
}

void TestDragSource::theHookIsOfferedCopyAndNothingElse()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("payslip.pdf")));

    DragSource* source = makeSource();
    source->start({ VfsUri::fromLocalPath(tree.absolute(QStringLiteral("payslip.pdf"))) });

    // Not "copy among others" and not "copy by default": copy alone. A receiver
    // that was offered a move would delete the source after taking the bytes.
    QCOMPARE(m_actions, Qt::DropActions(Qt::CopyAction));
    QVERIFY(!m_actions.testFlag(Qt::MoveAction));
    QVERIFY(!m_actions.testFlag(Qt::LinkAction));
}

void TestDragSource::rowsThatAreNotOnDiskStartNothingAndSayWhy()
{
    DragSource* source = makeSource();
    QSignalSpy refused(source, &DragSource::refused);
    QSignalSpy started(source, &DragSource::started);

    // What a selection inside a mounted archive looks like from here: valid
    // rows, on a drive, with no path any other application could open.
    source->start({
        VfsUri::fromString(QStringLiteral("zip:///backup.zip/notes/monday.txt")),
        VfsUri::fromString(QStringLiteral("zip:///backup.zip/notes/tuesday.txt")),
    });

    QCOMPARE(m_handovers, 0);
    QCOMPARE(started.count(), 0);
    QCOMPARE(refused.count(), 1);
    QVERIFY(!refused.first().first().toString().isEmpty());
}

void TestDragSource::aMixedSelectionSendsWhatItCanAndSaysHowManyStayed()
{
    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("here.txt")));

    DragSource* source = makeSource();
    QSignalSpy started(source, &DragSource::started);
    QSignalSpy leftBehind(source, &DragSource::leftBehind);

    source->start({
        VfsUri::fromLocalPath(tree.absolute(QStringLiteral("here.txt"))),
        VfsUri::fromString(QStringLiteral("sftp://nas/volume1/away.txt")),
        VfsUri::fromString(QStringLiteral("s3://bucket/reports/away.csv")),
    });

    // The local row goes, and the two that could not are counted out loud. Half
    // a selection leaving in silence is the one outcome this must not have.
    QCOMPARE(m_urls.size(), 1);
    QCOMPARE(m_urls.first().toLocalFile(), tree.absolute(QStringLiteral("here.txt")));
    QCOMPARE(started.count(), 1);
    QCOMPARE(leftBehind.count(), 1);
    QCOMPARE(leftBehind.first().at(0).toInt(), 1);
    QCOMPARE(leftBehind.first().at(1).toInt(), 2);
}

void TestDragSource::anEmptySelectionStartsNothing()
{
    DragSource* source = makeSource();
    QSignalSpy refused(source, &DragSource::refused);

    source->start({});

    QCOMPARE(m_handovers, 0);
    QCOMPARE(refused.count(), 1);
}

void TestDragSource::aRefusedDragSaysSo()
{
    // The platform declining the drag is a normal outcome and has to become a
    // message rather than silence.
    m_hookResult = false;

    TempTree tree;
    QVERIFY(tree.isValid());
    QVERIFY(tree.writeFile(QStringLiteral("thing.txt")));

    DragSource* source = makeSource();
    QSignalSpy refused(source, &DragSource::refused);
    QSignalSpy started(source, &DragSource::started);

    source->start({ VfsUri::fromLocalPath(tree.absolute(QStringLiteral("thing.txt"))) });

    QCOMPARE(m_handovers, 1);
    QCOMPARE(started.count(), 0);
    QCOMPARE(refused.count(), 1);
    QVERIFY(!refused.first().first().toString().isEmpty());
}

MOLE_TEST_MAIN(TestDragSource)
#include "tst_DragSource.moc"
