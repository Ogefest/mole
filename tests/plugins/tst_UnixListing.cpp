#include "plugins/network/UnixListing.h"
#include "support/MoleTestMain.h"

#include <QTimeZone>

using namespace mole;
using namespace mole::net;

namespace {

/// Fixed so a listing that omits the year has something to be resolved against
/// that a test can actually assert about.
QDateTime referenceNow()
{
    return QDateTime(QDate(2026, 8, 9), QTime(12, 0), QTimeZone::systemTimeZone());
}

} // namespace

class TestUnixListing : public QObject
{
    Q_OBJECT

private slots:
    void aFileLineIsRead();
    void aDirectoryLineIsRead();
    void aSymlinkKeepsOnlyItsOwnName();
    void aNameWithSpacesSurvives();
    void aYearIsUsedWhenGiven();
    void aTimeIsResolvedAgainstThisYear();
    void aTimeInTheFutureRollsBackAYear();
    void linesThatAreNotEntriesAreRejected();
    void dotEntriesAreKeptSoCallersCanReadThem();
    void aFileListedAsADirectoryIsRecognisable();
    void whatLibcurlProducesForSftpIsUnderstood();
};

void TestUnixListing::aFileLineIsRead()
{
    const ListingRow row = parseListingLine(
        QStringLiteral("-rw-r--r--   1 lukasz  staff   1234 Sep 16  2021 notes.txt"), referenceNow());

    QVERIFY(row.valid);
    QCOMPARE(row.name, QStringLiteral("notes.txt"));
    QVERIFY(!row.isDir);
    QVERIFY(!row.isSymlink);
    QCOMPARE(row.size, 1234);
    QCOMPARE(row.permissions, QStringLiteral("rw-r--r--"));
    QCOMPARE(row.owner, QStringLiteral("lukasz"));
    QCOMPARE(row.group, QStringLiteral("staff"));
}

void TestUnixListing::aDirectoryLineIsRead()
{
    const ListingRow row
        = parseListingLine(QStringLiteral("drwx------   1 -  -   0 Sep 16  2021 Shared"), referenceNow());

    QVERIFY(row.valid);
    QVERIFY(row.isDir);
    QCOMPARE(row.name, QStringLiteral("Shared"));
}

void TestUnixListing::aSymlinkKeepsOnlyItsOwnName()
{
    const ListingRow row = parseListingLine(
        QStringLiteral("lrwxrwxrwx   1 root root   7 Jan  3 09:15 latest -> reports/2026"), referenceNow());

    QVERIFY(row.valid);
    QVERIFY(row.isSymlink);
    // The arrow and everything after it belongs to the listing, not to the name.
    QCOMPARE(row.name, QStringLiteral("latest"));
}

void TestUnixListing::aNameWithSpacesSurvives()
{
    const ListingRow row = parseListingLine(
        QStringLiteral("-rw-r--r--   1 lukasz staff  42 Jan  3 09:15 quarterly report final.pdf"),
        referenceNow());

    QVERIFY(row.valid);
    QCOMPARE(row.name, QStringLiteral("quarterly report final.pdf"));
    QCOMPARE(row.size, 42);
}

void TestUnixListing::aYearIsUsedWhenGiven()
{
    const ListingRow row
        = parseListingLine(QStringLiteral("-rw-r--r--   1 a b  1 Sep 16  2021 old.txt"), referenceNow());

    QVERIFY(row.valid);
    QCOMPARE(row.modified.date(), QDate(2021, 9, 16));
}

void TestUnixListing::aTimeIsResolvedAgainstThisYear()
{
    const ListingRow row
        = parseListingLine(QStringLiteral("-rw-r--r--   1 a b  1 Aug  9 08:54 recent.txt"), referenceNow());

    QVERIFY(row.valid);
    QCOMPARE(row.modified.date(), QDate(2026, 8, 9));
    QCOMPARE(row.modified.time().hour(), 8);
    QCOMPARE(row.modified.time().minute(), 54);
}

void TestUnixListing::aTimeInTheFutureRollsBackAYear()
{
    // December with no year, seen in August, is last December rather than a file
    // modified four months from now.
    const ListingRow row = parseListingLine(
        QStringLiteral("-rw-r--r--   1 a b  1 Dec 24 18:00 christmas.txt"), referenceNow());

    QVERIFY(row.valid);
    QCOMPARE(row.modified.date(), QDate(2025, 12, 24));
}

void TestUnixListing::linesThatAreNotEntriesAreRejected()
{
    QVERIFY(!parseListingLine(QStringLiteral("total 48"), referenceNow()).valid);
    QVERIFY(!parseListingLine(QString(), referenceNow()).valid);
    QVERIFY(!parseListingLine(QStringLiteral("   "), referenceNow()).valid);
    QVERIFY(!parseListingLine(QStringLiteral("220 Welcome to the server"), referenceNow()).valid);
    // Enough columns, but the first one is not a mode string.
    QVERIFY(!parseListingLine(QStringLiteral("garbage 1 a b 0 Sep 16 2021 name"), referenceNow()).valid);
}

void TestUnixListing::dotEntriesAreKeptSoCallersCanReadThem()
{
    const QByteArray listing = "drw-------   1 -        -               0 Aug  9 08:54 .\n"
                               "d---------   1 -        -               0 Aug  9 08:54 ..\n"
                               "-rw-------   1 lukasz   -               5 Aug  9 08:54 alpha.txt\n";

    const QList<ListingRow> rows = parseUnixListing(listing, referenceNow());
    QCOMPARE(rows.size(), 3);

    int visible = 0;
    for (const ListingRow& row : rows) {
        if (!isDotEntry(row))
            ++visible;
    }
    QCOMPARE(visible, 1);
    QVERIFY(isDotEntry(rows.at(0)));
    QVERIFY(isDotEntry(rows.at(1)));
    QVERIFY(!isDotEntry(rows.at(2)));
}

void TestUnixListing::aFileListedAsADirectoryIsRecognisable()
{
    // Regression. Asked to list a regular file, an SFTP server answers with a
    // "." that describes the file rather than refusing outright. When the dot
    // rows were discarded this arrived as a successful, empty listing, so
    // browsing into a file showed an empty folder instead of an error.
    const QByteArray listing = "-rw-------   1 lukasz   -               5 Aug  9 08:57 .\n"
                               "d---------   1 -        -               0 Aug  9 08:57 ..\n";

    const QList<ListingRow> rows = parseUnixListing(listing, referenceNow());
    QCOMPARE(rows.size(), 2);

    const ListingRow& self = rows.at(0);
    QCOMPARE(self.name, QStringLiteral("."));
    QVERIFY2(!self.isDir, "the '.' row is what reveals that the target is a file");
    QCOMPARE(self.size, 5);
}

void TestUnixListing::whatLibcurlProducesForSftpIsUnderstood()
{
    // Copied verbatim from a real session against an SFTP server, which is the
    // only thing that proves the parser matches what libcurl actually emits
    // rather than what this file imagines it emits.
    const QByteArray listing = "dr--------   1 -        -               0 Sep 16  2021 Private\n"
                               "drw-------   1 -        -               0 Sep 16  2021 Shared\n";

    const QList<ListingRow> rows = parseUnixListing(listing, referenceNow());
    QCOMPARE(rows.size(), 2);
    QVERIFY(rows.at(0).isDir);
    QCOMPARE(rows.at(0).name, QStringLiteral("Private"));
    QCOMPARE(rows.at(0).permissions, QStringLiteral("r--------"));
    QVERIFY(rows.at(1).isDir);
    QCOMPARE(rows.at(1).name, QStringLiteral("Shared"));
    QCOMPARE(rows.at(1).permissions, QStringLiteral("rw-------"));
}

MOLE_TEST_MAIN(TestUnixListing)

#include "tst_UnixListing.moc"
