#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/search/LiveSearchTask.h"
#include "core/search/SearchQuery.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

using namespace mole;
using namespace mole::test;

/// Looking inside the files.
///
/// The missing half of a search tool: the name is what you have forgotten and
/// the contents are what you remember. The contents are deliberately never
/// indexed — a full-text index over a disk of files at scale is a second disk —
/// so this is a filter over candidates rather than a lookup, and everything
/// here follows from that: it goes last, it is bounded, and it says what it
/// left out.
class TestContentSearch : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aLiteralIsFoundWithItsLineAndItsPlaceInIt();
    void anExpressionIsMatchedWhenAskedForAndNotOtherwise();
    void caseAndWholeWordsEachNarrowIt();
    void aMatchAcrossAWindowBoundaryIsStillFound();
    void aBinaryFileIsSkippedUnlessAskedForAndTheSnifferDecides();
    void aFileOverTheCeilingIsSkippedAndSaidToBe();
    void aUtf16FileIsSearchedAndAMangledOneIsSkipped();
    void cancellingStopsTheReading();
    void contentIsEvaluatedLastSoOnlyThePdfsAreOpened();

private:
    /// A reader over a set of files held in memory, counting what it opened.
    SearchIo ioOver(const QHash<QString, QByteArray>& files);
    FileEntry entryFor(const QString& uri, const QHash<QString, QByteArray>& files) const;

    mutable QStringList m_opened;
    std::unique_ptr<TaskManager> m_tasks;
    std::shared_ptr<MemoryFileSystem> m_fs;
    FileEntryList m_hits;
    QList<ContentMatch> m_reasons;
};

void TestContentSearch::init()
{
    m_opened.clear();
    m_tasks = std::make_unique<TaskManager>();
    m_fs = std::make_shared<MemoryFileSystem>();
    m_hits.clear();
    m_reasons.clear();
}

void TestContentSearch::cleanup()
{
    m_tasks.reset();
    m_fs.reset();
}

SearchIo TestContentSearch::ioOver(const QHash<QString, QByteArray>& files)
{
    SearchIo io;
    io.read = [this, files](const VfsUri& uri, qint64 offset, qint64 bytes) -> QByteArray {
        m_opened.append(uri.toString());
        const QByteArray whole = files.value(uri.toString());
        if (offset >= whole.size())
            return {};
        return whole.mid(static_cast<qsizetype>(offset), static_cast<qsizetype>(bytes));
    };
    return io;
}

FileEntry TestContentSearch::entryFor(const QString& uri, const QHash<QString, QByteArray>& files) const
{
    FileEntry entry;
    entry.uri = VfsUri::fromString(uri);
    entry.name = entry.uri.fileName();
    entry.size = files.value(uri).size();
    return entry;
}

void TestContentSearch::aLiteralIsFoundWithItsLineAndItsPlaceInIt()
{
    const QHash<QString, QByteArray> files {
        { QStringLiteral("mem:///notes.txt"),
            QByteArrayLiteral("first line\nsecond line\n    the invoice number is 4471\nlast\n") },
        { QStringLiteral("mem:///other.txt"), QByteArrayLiteral("nothing of interest here\n") },
    };
    const SearchIo io = ioOver(files);

    const SearchPredicate looking = SearchPredicate::content(QStringLiteral("invoice"));
    ContentMatch why;
    QVERIFY(looking.matches(entryFor(QStringLiteral("mem:///notes.txt"), files), io, &why));

    // The line, so nobody has to open the file to find out which it meant.
    QCOMPARE(why.lineNumber, 3);
    QCOMPARE(why.line, QStringLiteral("the invoice number is 4471"));
    QCOMPARE(why.length, 7);
    // Counted in the trimmed line, which is the one on the screen.
    QCOMPARE(why.line.mid(why.column, why.length), QStringLiteral("invoice"));

    ContentMatch none;
    QVERIFY(!looking.matches(entryFor(QStringLiteral("mem:///other.txt"), files), io, &none));
    QVERIFY(!none.isValid());
}

void TestContentSearch::anExpressionIsMatchedWhenAskedForAndNotOtherwise()
{
    const QHash<QString, QByteArray> files {
        { QStringLiteral("mem:///a.txt"), QByteArrayLiteral("order INV-2026-0042 shipped\n") },
        { QStringLiteral("mem:///b.txt"), QByteArrayLiteral("order INV-XXXX-YYYY shipped\n") },
    };
    const SearchIo io = ioOver(files);

    const SearchPredicate expression
        = SearchPredicate::content(QStringLiteral("INV-[0-9]{4}-[0-9]{4}"), true);
    QVERIFY(expression.matches(entryFor(QStringLiteral("mem:///a.txt"), files), io));
    QVERIFY(!expression.matches(entryFor(QStringLiteral("mem:///b.txt"), files), io));

    // The same text as a literal is a literal: nothing in the file says
    // "INV-[0-9]{4}", so a search that quietly compiled it would be answering a
    // question nobody asked.
    const SearchPredicate literal = SearchPredicate::content(QStringLiteral("INV-[0-9]{4}-[0-9]{4}"));
    QVERIFY(!literal.matches(entryFor(QStringLiteral("mem:///a.txt"), files), io));
}

void TestContentSearch::caseAndWholeWordsEachNarrowIt()
{
    const QHash<QString, QByteArray> files {
        { QStringLiteral("mem:///a.txt"), QByteArrayLiteral("The Report was filed\n") },
        { QStringLiteral("mem:///b.txt"), QByteArrayLiteral("reporting is not the same word\n") },
    };
    const SearchIo io = ioOver(files);
    const FileEntry a = entryFor(QStringLiteral("mem:///a.txt"), files);
    const FileEntry b = entryFor(QStringLiteral("mem:///b.txt"), files);

    QVERIFY(SearchPredicate::content(QStringLiteral("report")).matches(a, io));
    QVERIFY(!SearchPredicate::content(QStringLiteral("report"), false, true).matches(a, io));
    QVERIFY(SearchPredicate::content(QStringLiteral("Report"), false, true).matches(a, io));

    SearchPredicate word = SearchPredicate::content(QStringLiteral("report"));
    word.wholeWord = true;
    QVERIFY(word.matches(a, io));
    QVERIFY2(!word.matches(b, io), "whole words has to stop report matching reporting inside a file too");
    QVERIFY(SearchPredicate::content(QStringLiteral("report")).matches(b, io));
}

/// The test that fails the moment the overlap between windows is dropped.
void TestContentSearch::aMatchAcrossAWindowBoundaryIsStillFound()
{
    const QByteArray needle = QByteArrayLiteral("SPANNING-THE-BOUNDARY");
    // Placed so that the window edge falls in the middle of it.
    const qint64 start = SearchIo::kWindowBytes - (needle.size() / 2);
    QByteArray whole(SearchIo::kWindowBytes * 2, 'x');
    whole.replace(static_cast<qsizetype>(start), needle.size(), needle);
    // A newline before it, so the line the match is on is a line and not the
    // whole file.
    whole[static_cast<qsizetype>(start) - 1] = '\n';

    const QHash<QString, QByteArray> files { { QStringLiteral("mem:///big.txt"), whole } };
    const SearchIo io = ioOver(files);

    ContentMatch why;
    QVERIFY2(SearchPredicate::content(QStringLiteral("SPANNING-THE-BOUNDARY"))
                 .matches(entryFor(QStringLiteral("mem:///big.txt"), files), io, &why),
        "a match lying across a window edge is exactly what the overlap is for");
    QVERIFY(why.isValid());
    QVERIFY(why.line.contains(QStringLiteral("SPANNING-THE-BOUNDARY")));
}

void TestContentSearch::aBinaryFileIsSkippedUnlessAskedForAndTheSnifferDecides()
{
    // The word is in both, and one of them is a binary file wearing a .txt.
    QByteArray binary = QByteArrayLiteral("\x7f"
                                          "ELF\x02\x01\x01");
    binary.append(QByteArray(64, '\0'));
    binary.append(QByteArrayLiteral("secretword"));
    binary.append(QByteArray(64, '\0'));

    const QHash<QString, QByteArray> files {
        { QStringLiteral("mem:///program.txt"), binary },
        { QStringLiteral("mem:///notes.dat"), QByteArrayLiteral("plainly a secretword in text\n") },
    };
    const SearchIo io = ioOver(files);

    const SearchPredicate text = SearchPredicate::content(QStringLiteral("secretword"));
    QVERIFY2(!text.matches(entryFor(QStringLiteral("mem:///program.txt"), files), io),
        "a binary file is skipped whatever its suffix says");
    QVERIFY2(text.matches(entryFor(QStringLiteral("mem:///notes.dat"), files), io),
        "and a text file is searched whatever its suffix says");

    SearchPredicate bytes = text;
    bytes.includeBinary = true;
    QVERIFY(bytes.matches(entryFor(QStringLiteral("mem:///program.txt"), files), io));

    // And the reason it was skipped is available rather than guessed at.
    ContentSkip why = ContentSkip::None;
    findInContents(entryFor(QStringLiteral("mem:///program.txt"), files), text, io, &why);
    QCOMPARE(why, ContentSkip::Binary);
}

void TestContentSearch::aFileOverTheCeilingIsSkippedAndSaidToBe()
{
    const QHash<QString, QByteArray> files { { QStringLiteral("mem:///huge.log"),
        QByteArrayLiteral("the needle is in here\n") } };
    SearchIo io = ioOver(files);
    io.ceiling = 8; // a stand-in for the disk image nobody meant to search

    FileEntry huge = entryFor(QStringLiteral("mem:///huge.log"), files);
    huge.size = 40LL * 1024 * 1024 * 1024;

    ContentSkip why = ContentSkip::None;
    const ContentMatch found
        = findInContents(huge, SearchPredicate::content(QStringLiteral("needle")), io, &why);
    QVERIFY(!found.isValid());
    QCOMPARE(why, ContentSkip::TooBig);
    QVERIFY2(m_opened.isEmpty(), "a file over the ceiling must not be opened at all");
}

void TestContentSearch::aUtf16FileIsSearchedAndAMangledOneIsSkipped()
{
    QByteArray utf16 = QByteArrayLiteral("\xff\xfe");
    for (const QChar ch : QStringLiteral("the invoice number\n")) {
        utf16.append(static_cast<char>(ch.unicode() & 0xff));
        utf16.append(static_cast<char>(ch.unicode() >> 8));
    }

    QByteArray withBom = QByteArrayLiteral("\xef\xbb\xbf");
    withBom.append(QByteArrayLiteral("the invoice number\n"));

    // Text by the sniffer's reckoning and not valid UTF-8: bytes in the C1
    // range with no NUL and few controls. Mangling it into replacement
    // characters and searching that would be worse than not searching it.
    QByteArray broken = QByteArrayLiteral("plain enough to look like text ");
    broken.append(QByteArray(40, static_cast<char>(0xC3)));
    broken.append('\n');

    const QHash<QString, QByteArray> files {
        { QStringLiteral("mem:///wide.txt"), utf16 },
        { QStringLiteral("mem:///bom.txt"), withBom },
        { QStringLiteral("mem:///broken.txt"), broken },
    };
    const SearchIo io = ioOver(files);
    const SearchPredicate looking = SearchPredicate::content(QStringLiteral("invoice"));

    QVERIFY(looking.matches(entryFor(QStringLiteral("mem:///wide.txt"), files), io));
    QVERIFY(looking.matches(entryFor(QStringLiteral("mem:///bom.txt"), files), io));

    ContentSkip why = ContentSkip::None;
    findInContents(entryFor(QStringLiteral("mem:///broken.txt"), files),
        SearchPredicate::content(QStringLiteral("plain")), io, &why);
    QVERIFY2(why == ContentSkip::Undecodable || why == ContentSkip::Binary,
        "a file that will not decode is left alone rather than searched as nonsense");
}

void TestContentSearch::cancellingStopsTheReading()
{
    // Big enough to need several windows, so there is a between-windows moment
    // for the cancel to be noticed at.
    QByteArray whole(SearchIo::kWindowBytes * 4, 'x');
    whole.append(QByteArrayLiteral("\nthe needle at the very end\n"));
    const QHash<QString, QByteArray> files { { QStringLiteral("mem:///long.txt"), whole } };

    SearchIo io = ioOver(files);
    bool stopped = false;
    // Called off the moment the first window has been handed over: on the
    // condition, not on a clock.
    io.cancelled = [this, &stopped] {
        if (m_opened.size() >= 2)
            stopped = true;
        return stopped;
    };

    const ContentMatch found = findInContents(entryFor(QStringLiteral("mem:///long.txt"), files),
        SearchPredicate::content(QStringLiteral("needle")), io);
    QVERIFY2(!found.isValid(), "a cancelled read must not go on to answer");
    QVERIFY2(stopped, "the cancel was never even asked about");
    QVERIFY2(m_opened.size() < 5, "the reading carried on past the cancel");
}

/// The cost ladder, doing the thing it exists for.
void TestContentSearch::contentIsEvaluatedLastSoOnlyThePdfsAreOpened()
{
    const QHash<QString, QByteArray> files {
        { QStringLiteral("mem:///a.pdf"), QByteArrayLiteral("a document mentioning invoice 12\n") },
        { QStringLiteral("mem:///b.pdf"), QByteArrayLiteral("a document mentioning nothing\n") },
        { QStringLiteral("mem:///c.txt"), QByteArrayLiteral("a note mentioning invoice 12\n") },
        { QStringLiteral("mem:///d.log"), QByteArrayLiteral("a log mentioning invoice 12\n") },
    };
    const SearchIo io = ioOver(files);

    SearchQuery query;
    query.add(SearchPredicate::content(QStringLiteral("invoice")));
    query.add(SearchPredicate::extensions({ QStringLiteral("pdf") }));

    // Written content-first on purpose; the plan reorders it.
    const SearchPlan plan = planSearch(query, SearchSource::Walk);
    QCOMPARE(plan.remainder().size(), 2);
    QCOMPARE(plan.remainder().first().field, SearchPredicate::Field::Extension);
    QCOMPARE(plan.remainder().last().field, SearchPredicate::Field::Content);

    QStringList hits;
    for (const QString& uri : files.keys()) {
        if (plan.matches(entryFor(uri, files), io))
            hits.append(uri);
    }
    QCOMPARE(hits, QStringList { QStringLiteral("mem:///a.pdf") });

    // And the .txt and the .log were never opened, which is the whole point:
    // filter to the PDFs first, then read them.
    for (const QString& opened : m_opened) {
        QVERIFY2(opened.endsWith(QStringLiteral(".pdf")),
            qPrintable(QStringLiteral("%1 was opened by a search for PDFs").arg(opened)));
    }
    QVERIFY(!m_opened.isEmpty());
}

MOLE_TEST_MAIN(TestContentSearch)
#include "tst_ContentSearch.moc"
