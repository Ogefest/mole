#include "support/MoleTestMain.h"

#include "core/vfs/VfsUri.h"

#include <QSet>

using namespace mole;

Q_DECLARE_METATYPE(mole::HostPlatform)

class TestVfsUri : public QObject
{
    Q_OBJECT

private slots:
    void defaultConstructedIsInvalid();

    void parses_data();
    void parses();

    void normalisesPath_data();
    void normalisesPath();

    void childAndParent();
    void aChildCannotClimbOutOfItsParent();
    void parentOfRootIsRoot();
    void suffix_data();
    void suffix();
    void isWithin();

    void aDriveLetterSurvivesTheUriLayer();
    void walkingUpStopsAtTheDriveRoot();
    void aUncShareSurvivesTheUriLayer();
    void walkingUpStopsAtTheShare();
    void dotDotCannotClimbOutOfAVolume();
    void aColonInANameIsNotADrive();
    void nativeSpellingIsThePlatformsOwn_data();
    void nativeSpellingIsThePlatformsOwn();
    void aShareHasNoPosixNativePath();

    void caseFoldingIsAnArgumentAndFoldsEverything();
    void caseSensitivityFollowsTheSchemeAndThePlatform();
    void aCanonicalKeyIsOneSpellingPerNode();

    void aVersionIsPartOfTheUriAndSurvivesBeingWrittenDown();
    void aVersionIsPartOfWhatMakesTwoUrisDifferent();
    void aQuestionMarkInANameIsNotAVersion();
    void aVersionBelongsToAFileAndNotToWhatIsAroundIt();
    void aUriWrittenBeforeVersionsExistedStillReadsTheSame();

    void localPathRoundTrip();
    void nonLocalHasNoLocalPath();
    void equalityAndHashing();
};

void TestVfsUri::defaultConstructedIsInvalid()
{
    const VfsUri uri;
    QVERIFY(!uri.isValid());
    QVERIFY(uri.toString().isEmpty());
}

void TestVfsUri::parses_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<QString>("scheme");
    QTest::addColumn<QString>("authority");
    QTest::addColumn<QString>("path");

    QTest::newRow("local file") << "file:///home/user/a.txt" << true << "file" << "" << "/home/user/a.txt";
    QTest::newRow("sftp with user") << "sftp://user@nas.local/volume1/photos" << true << "sftp"
                                    << "user@nas.local" << "/volume1/photos";
    QTest::newRow("s3 bucket") << "s3://my-bucket/reports" << true << "s3" << "my-bucket" << "/reports";
    QTest::newRow("authority only") << "s3://my-bucket" << true << "s3" << "my-bucket" << "/";
    QTest::newRow("uppercase scheme") << "FILE:///tmp" << true << "file" << "" << "/tmp";
    QTest::newRow("no scheme") << "/home/user" << false << "" << "" << "";
    QTest::newRow("empty") << "" << false << "" << "" << "";
}

void TestVfsUri::parses()
{
    QFETCH(QString, input);
    QFETCH(bool, valid);
    QFETCH(QString, scheme);
    QFETCH(QString, authority);
    QFETCH(QString, path);

    const VfsUri uri = VfsUri::fromString(input);
    QCOMPARE(uri.isValid(), valid);
    if (!valid)
        return;

    QCOMPARE(uri.scheme(), scheme);
    QCOMPARE(uri.authority(), authority);
    QCOMPARE(uri.path(), path);
}

void TestVfsUri::normalisesPath_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("trailing slash") << "/home/user/" << "/home/user";
    QTest::newRow("double slash") << "/home//user" << "/home/user";
    QTest::newRow("dot segment") << "/home/./user" << "/home/user";
    QTest::newRow("dotdot segment") << "/home/user/../root" << "/home/root";
    QTest::newRow("dotdot past root") << "/../../etc" << "/etc";
    // A backslash is an ordinary character in a name everywhere except Windows,
    // and a uri is not a native path: turning one into a separator here made
    // "back\\slash.txt" into a file in a directory nobody has.
    QTest::newRow("a backslash is part of the name") << "/home/back\\slash.txt" << "/home/back\\slash.txt";
    QTest::newRow("bare root") << "/" << "/";
    QTest::newRow("empty becomes root") << "" << "/";
}

void TestVfsUri::normalisesPath()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);
    QCOMPARE(VfsUri(QStringLiteral("file"), QString(), input).path(), expected);
}

void TestVfsUri::childAndParent()
{
    const VfsUri root = VfsUri::fromString(QStringLiteral("file:///home"));
    const VfsUri child = root.child(QStringLiteral("user"));

    QCOMPARE(child.toString(), QStringLiteral("file:///home/user"));
    QCOMPARE(child.fileName(), QStringLiteral("user"));
    QCOMPARE(child.parent(), root);

    // Chaining from the root must not produce a doubled slash.
    const VfsUri fromRoot = VfsUri::fromString(QStringLiteral("file:///")).child(QStringLiteral("tmp"));
    QCOMPARE(fromRoot.toString(), QStringLiteral("file:///tmp"));
}

/// child() is where a listing row becomes a uri, and it could climb.
///
/// The constructor normalises "..", so `dir.child("../x")` *was*
/// `dir.parent().child("x")` -- the row walking out of the tree it was listed
/// from. The archive backend and TransferTask sanitise their inputs; SyncPlan's
/// walk and LocalFileSystem's listing trust theirs, so the seam was one backend
/// away from being walked through. Closed here so it cannot be reopened by a
/// backend written next year. See MOLE-359.
void TestVfsUri::aChildCannotClimbOutOfItsParent()
{
    const VfsUri dir = VfsUri::fromString(QStringLiteral("mem:///papers/2026"));

    for (const QString& name : { QStringLiteral(".."), QStringLiteral("../x"), QStringLiteral("a/../../x"),
             QStringLiteral("x/.."), QStringLiteral("."), QStringLiteral("./"), QStringLiteral("/") }) {
        const VfsUri child = dir.child(name);
        QVERIFY2(!child.isValid(),
            qPrintable(QStringLiteral("child(\"%1\") gave %2").arg(name, child.toString())));
    }

    // A relative path is still a child, because that is what an archive entry
    // is, and it lands underneath.
    const VfsUri deep = dir.child(QStringLiteral("docs/notes/report.txt"));
    QVERIFY(deep.isValid());
    QCOMPARE(deep.path(), QStringLiteral("/papers/2026/docs/notes/report.txt"));
    QVERIFY(deep.isWithin(dir));

    // And an ordinary name is untouched, including one with dots in it.
    QCOMPARE(dir.child(QStringLiteral("..hidden")).path(), QStringLiteral("/papers/2026/..hidden"));
    QCOMPARE(dir.child(QStringLiteral("a..b")).path(), QStringLiteral("/papers/2026/a..b"));
    QCOMPARE(dir.child(QStringLiteral("report.txt")).path(), QStringLiteral("/papers/2026/report.txt"));
}

void TestVfsUri::parentOfRootIsRoot()
{
    const VfsUri root = VfsUri::fromString(QStringLiteral("s3://bucket/"));
    QVERIFY(root.isRoot());
    QCOMPARE(root.parent(), root);
    QVERIFY(root.fileName().isEmpty());
}

void TestVfsUri::suffix_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("expected");

    QTest::newRow("simple") << "/a/report.PDF" << "pdf";
    QTest::newRow("double extension") << "/a/archive.tar.gz" << "gz";
    QTest::newRow("no extension") << "/a/README" << "";
    QTest::newRow("dotfile") << "/a/.bashrc" << "";
    QTest::newRow("trailing dot") << "/a/weird." << "";
}

void TestVfsUri::suffix()
{
    QFETCH(QString, path);
    QFETCH(QString, expected);
    QCOMPARE(VfsUri(QStringLiteral("file"), QString(), path).suffix(), expected);
}

void TestVfsUri::isWithin()
{
    const VfsUri root = VfsUri::fromString(QStringLiteral("file:///home"));
    QVERIFY(VfsUri::fromString(QStringLiteral("file:///home/user/a.txt")).isWithin(root));
    QVERIFY(root.isWithin(root));

    // A shared prefix is not containment.
    QVERIFY(!VfsUri::fromString(QStringLiteral("file:///homeless")).isWithin(root));
    // Different scheme or authority never matches.
    QVERIFY(!VfsUri::fromString(QStringLiteral("sftp://h/home/user")).isWithin(root));
    QVERIFY(VfsUri::fromString(QStringLiteral("file:///anything"))
                .isWithin(VfsUri::fromString(QStringLiteral("file:///"))));
}

/// Everything below is the uri half, which has no platform in it: it is string
/// handling, so it is asserted on every machine the suite runs on. The native
/// half comes after, and takes the platform as an argument for the same reason.

void TestVfsUri::aDriveLetterSurvivesTheUriLayer()
{
    const VfsUri uri = VfsUri::fromString(QStringLiteral("file:///C:/Users/ann/notes.TXT"));

    QCOMPARE(uri.scheme(), QStringLiteral("file"));
    QVERIFY(uri.authority().isEmpty());
    QCOMPARE(uri.path(), QStringLiteral("/C:/Users/ann/notes.TXT"));
    QCOMPARE(uri.toString(), QStringLiteral("file:///C:/Users/ann/notes.TXT"));
    QCOMPARE(uri.fileName(), QStringLiteral("notes.TXT"));
    QCOMPARE(uri.suffix(), QStringLiteral("txt"));

    const VfsUri driveRoot = VfsUri::fromString(QStringLiteral("file:///C:"));
    QVERIFY(uri.isWithin(driveRoot));
    QVERIFY(!uri.isWithin(VfsUri::fromString(QStringLiteral("file:///D:"))));

    // A child of the drive root keeps the drive. Losing it here would put every
    // listing of C:\ onto a volume nobody asked for.
    QCOMPARE(driveRoot.child(QStringLiteral("Users")).toString(), QStringLiteral("file:///C:/Users"));
}

void TestVfsUri::walkingUpStopsAtTheDriveRoot()
{
    VfsUri uri = VfsUri::fromString(QStringLiteral("file:///C:/Users/ann"));

    QCOMPARE(uri.parent().toString(), QStringLiteral("file:///C:/Users"));
    uri = uri.parent().parent();
    QCOMPARE(uri.toString(), QStringLiteral("file:///C:"));

    // And there it stops. "/" is not a place on Windows and no backend can list
    // it, so producing one would be worse than staying put.
    QVERIFY(uri.isRoot());
    QCOMPARE(uri.parent(), uri);
    QVERIFY(uri.fileName().isEmpty());
}

void TestVfsUri::aUncShareSurvivesTheUriLayer()
{
    const VfsUri uri = VfsUri::fromString(QStringLiteral("file://server/share/dir/report.pdf"));

    QCOMPARE(uri.authority(), QStringLiteral("server"));
    QCOMPARE(uri.path(), QStringLiteral("/share/dir/report.pdf"));
    QCOMPARE(uri.toString(), QStringLiteral("file://server/share/dir/report.pdf"));
    QCOMPARE(uri.fileName(), QStringLiteral("report.pdf"));
    QCOMPARE(uri.suffix(), QStringLiteral("pdf"));

    // Two servers are two places however alike the paths look.
    QVERIFY(!uri.isWithin(VfsUri::fromString(QStringLiteral("file://other/share"))));
    QVERIFY(uri.isWithin(VfsUri::fromString(QStringLiteral("file://server/share"))));
}

void TestVfsUri::walkingUpStopsAtTheShare()
{
    VfsUri uri = VfsUri::fromString(QStringLiteral("file://server/share/dir"));

    uri = uri.parent();
    QCOMPARE(uri.toString(), QStringLiteral("file://server/share"));
    QVERIFY(uri.isRoot());
    QCOMPARE(uri.parent(), uri);

    QCOMPARE(uri.child(QStringLiteral("dir")).toString(), QStringLiteral("file://server/share/dir"));
}

void TestVfsUri::dotDotCannotClimbOutOfAVolume()
{
    // POSIX clamps at "/" and always has. A drive and a share clamp at
    // themselves, because there is nothing above either to arrive at.
    QCOMPARE(VfsUri::fromString(QStringLiteral("file:///C:/Users/../..")).toString(),
        QStringLiteral("file:///C:"));
    QCOMPARE(VfsUri::fromString(QStringLiteral("file://server/share/dir/../..")).toString(),
        QStringLiteral("file://server/share"));
    QCOMPARE(VfsUri::fromString(QStringLiteral("file:///home/../..")).toString(), QStringLiteral("file:///"));
}

void TestVfsUri::aColonInANameIsNotADrive()
{
    // "notes:" is a legal name on Linux and a directory somebody may really
    // have. Reading it as a drive would leave it with no name and nothing above
    // it, so the test is a letter and a colon rather than "ends with a colon".
    const VfsUri uri = VfsUri::fromString(QStringLiteral("file:///notes:"));
    QVERIFY(!uri.isRoot());
    QCOMPARE(uri.fileName(), QStringLiteral("notes:"));
    QCOMPARE(uri.parent().toString(), QStringLiteral("file:///"));

    // Nor is a colon anywhere but the first segment.
    const VfsUri deeper = VfsUri::fromString(QStringLiteral("file:///home/C:/x"));
    QCOMPARE(deeper.parent().parent().toString(), QStringLiteral("file:///home"));
}

void TestVfsUri::nativeSpellingIsThePlatformsOwn_data()
{
    QTest::addColumn<HostPlatform>("platform");
    QTest::addColumn<QString>("native");
    QTest::addColumn<QString>("uri");

    QTest::newRow("posix path") << HostPlatform::Posix << "/home/user/notes.txt"
                                << "file:///home/user/notes.txt";
    QTest::newRow("macos path") << HostPlatform::MacOS << "/Volumes/Backup/notes.txt"
                                << "file:///Volumes/Backup/notes.txt";
    QTest::newRow("windows drive") << HostPlatform::Windows << "C:\\Users\\ann\\notes.txt"
                                   << "file:///C:/Users/ann/notes.txt";
    QTest::newRow("windows share") << HostPlatform::Windows << "\\\\server\\share\\a.txt"
                                   << "file://server/share/a.txt";
    QTest::newRow("windows drive root") << HostPlatform::Windows << "C:\\" << "file:///C:";
    // A backslash is an ordinary character in a name here, and turning one into
    // a separator made "back\slash.txt" into a file in a directory nobody has.
    QTest::newRow("a backslash is part of the name")
        << HostPlatform::Posix << "/home/back\\slash.txt" << "file:///home/back\\slash.txt";
}

void TestVfsUri::nativeSpellingIsThePlatformsOwn()
{
    QFETCH(HostPlatform, platform);
    QFETCH(QString, native);
    QFETCH(QString, uri);

    // Both directions, on any machine, because the platform is an argument
    // rather than an #ifdef. The Windows rows are the ones that have never been
    // asserted anywhere.
    QCOMPARE(VfsUri::fromLocalPath(native, platform).toString(), uri);
    QCOMPARE(VfsUri::fromString(uri).toLocalPath(platform), native);
}

void TestVfsUri::aShareHasNoPosixNativePath()
{
    // Handing back "/share/a.txt" would name a local directory that has nothing
    // to do with the server.
    const VfsUri share = VfsUri::fromString(QStringLiteral("file://server/share/a.txt"));
    QVERIFY(share.toLocalPath(HostPlatform::Posix).isEmpty());
    QVERIFY(share.toLocalPath(HostPlatform::MacOS).isEmpty());
}

void TestVfsUri::caseFoldingIsAnArgumentAndFoldsEverything()
{
    const VfsUri upper = VfsUri::fromString(QStringLiteral("file:///C:/Users/Ann"));
    const VfsUri lower = VfsUri::fromString(QStringLiteral("file:///c:/users/ann"));

    // With folding on -- an NTFS volume, or a default APFS one -- these are one
    // directory, and every guard that asks "are these the same node" has to say
    // so. Asserted on every platform, because the sensitivity is an argument.
    QVERIFY(upper.equals(lower, Qt::CaseInsensitive));
    QVERIFY(lower.equals(upper, Qt::CaseInsensitive));
    QCOMPARE(upper.hash(0, Qt::CaseInsensitive), lower.hash(0, Qt::CaseInsensitive));

    // Containment has to fold the same way, or equality and containment
    // disagree about the same pair.
    const VfsUri deep = VfsUri::fromString(QStringLiteral("file:///C:/USERS/ann/notes.txt"));
    QVERIFY(deep.isWithin(lower, Qt::CaseInsensitive));
    QVERIFY(!deep.isWithin(lower, Qt::CaseSensitive));

    // With it off, none of it holds -- which is an ext4 disk, and an S3 bucket
    // wherever it is mounted.
    QVERIFY(!upper.equals(lower, Qt::CaseSensitive));
    QVERIFY(upper.hash(0, Qt::CaseSensitive) != lower.hash(0, Qt::CaseSensitive));

    // A different name is still a different name, folding or not.
    const VfsUri other = VfsUri::fromString(QStringLiteral("file:///C:/Users/Bob"));
    QVERIFY(!upper.equals(other, Qt::CaseInsensitive));
    QVERIFY(!other.isWithin(lower, Qt::CaseInsensitive));

    // The authority folds too: two spellings of one server are one server.
    QVERIFY(VfsUri::fromString(QStringLiteral("file://SERVER/Share/a"))
                .equals(VfsUri::fromString(QStringLiteral("file://server/share/a")), Qt::CaseInsensitive));
}

void TestVfsUri::caseSensitivityFollowsTheSchemeAndThePlatform()
{
    const QString local = QStringLiteral("file");

    QCOMPARE(VfsUri::caseSensitivityFor(local, HostPlatform::Posix), Qt::CaseSensitive);
    QCOMPARE(VfsUri::caseSensitivityFor(local, HostPlatform::Windows), Qt::CaseInsensitive);
    QCOMPARE(VfsUri::caseSensitivityFor(local, HostPlatform::MacOS), Qt::CaseInsensitive);

    // A bucket is case-sensitive wherever the client happens to be running, and
    // so is an SFTP server. Folding those because the machine folds would make
    // two real objects into one.
    for (const QString& remote :
        { QStringLiteral("s3"), QStringLiteral("sftp"), QStringLiteral("webdav"), QStringLiteral("mem") }) {
        QCOMPARE(VfsUri::caseSensitivityFor(remote, HostPlatform::Windows), Qt::CaseSensitive);
        QCOMPARE(VfsUri::caseSensitivityFor(remote, HostPlatform::Posix), Qt::CaseSensitive);
    }
}

void TestVfsUri::aCanonicalKeyIsOneSpellingPerNode()
{
    // Anything that keys by the text of a uri -- the analysis store hashes one
    // to name its folder -- has to agree with equals(), or one folder reached
    // two ways grows two stores that never agree.
    const VfsUri a = VfsUri::fromString(QStringLiteral("s3://bucket/Reports"));
    const VfsUri b = VfsUri::fromString(QStringLiteral("s3://bucket/reports"));
    QVERIFY(!a.equals(b));
    QVERIFY(a.canonicalKey() != b.canonicalKey());

    const VfsUri c = VfsUri::fromString(QStringLiteral("file:///home/./user/"));
    const VfsUri d = VfsUri::fromString(QStringLiteral("file:///home/user"));
    QVERIFY(c.equals(d));
    QCOMPARE(c.canonicalKey(), d.canonicalKey());
}

/// The whole point of putting it in the uri: a bookmark, a restored session and
/// a file set are all a string, and an earlier version has to survive being one.
void TestVfsUri::aVersionIsPartOfTheUriAndSurvivesBeingWrittenDown()
{
    const VfsUri current = VfsUri::fromString(QStringLiteral("s3://bucket/reports/q3.pdf"));
    QVERIFY(!current.hasVersion());

    const VfsUri earlier
        = current.withVersion(QStringLiteral("3HL4kqtJlcpXroDTDmJ+rmSpXd3dIbrHY+MTRCxf3vjVBH40Nrjfkd"));
    QVERIFY(earlier.hasVersion());
    QCOMPARE(earlier.path(), current.path());
    QCOMPARE(earlier.fileName(), QStringLiteral("q3.pdf"));
    QCOMPARE(earlier.suffix(), QStringLiteral("pdf"));

    QCOMPARE(VfsUri::fromString(earlier.toString()), earlier);
    QCOMPARE(VfsUri::fromString(earlier.toString()).version(), earlier.version());
    QCOMPARE(earlier.withoutVersion(), current);

    // A token with the characters that would otherwise end the uri, because a
    // drive's own identifier is opaque and nothing says it is tidy.
    const VfsUri awkward = current.withVersion(QStringLiteral("100%?version=no"));
    QCOMPARE(VfsUri::fromString(awkward.toString()), awkward);
    QCOMPARE(VfsUri::fromString(awkward.toString()).version(), QStringLiteral("100%?version=no"));
}

void TestVfsUri::aVersionIsPartOfWhatMakesTwoUrisDifferent()
{
    const VfsUri current = VfsUri::fromString(QStringLiteral("mem:///notes.txt"));
    const VfsUri earlier = current.withVersion(QStringLiteral("one"));
    const VfsUri earlierStill = current.withVersion(QStringLiteral("two"));

    QVERIFY(!(earlier == current));
    QVERIFY(!(earlier == earlierStill));
    QVERIFY(earlier == current.withVersion(QStringLiteral("one")));

    // The hash has to agree with the comparison beside it, or a QHash loses an
    // entry it is still holding -- and a map of open previews is one.
    QSet<VfsUri> seen { current, earlier, earlierStill };
    QCOMPARE(seen.size(), 3);
    QVERIFY(seen.contains(current.withVersion(QStringLiteral("two"))));

    QVERIFY(earlier.canonicalKey() != current.canonicalKey());
}

/// '?' is a legal character in a POSIX filename -- the awkward-names suite has a
/// really?.txt -- so the marker cannot simply be one.
void TestVfsUri::aQuestionMarkInANameIsNotAVersion()
{
    const VfsUri awkward = VfsUri::fromString(QStringLiteral("file:///home/ann/really?.txt"));
    QCOMPARE(awkward.fileName(), QStringLiteral("really?.txt"));
    QVERIFY(!awkward.hasVersion());
    QCOMPARE(VfsUri::fromString(awkward.toString()), awkward);
    QCOMPARE(VfsUri::fromString(awkward.toString()).fileName(), QStringLiteral("really?.txt"));

    // Including the name somebody would have to have chosen on purpose.
    const VfsUri worse = VfsUri::fromLocalPath(QStringLiteral("/home/ann/what?version=1.txt"));
    QVERIFY(!worse.hasVersion());
    QCOMPARE(VfsUri::fromString(worse.toString()), worse);
    QCOMPARE(VfsUri::fromString(worse.toString()).fileName(), QStringLiteral("what?version=1.txt"));

    // And a percent, which is what makes the encoding reversible at all.
    const VfsUri percent = VfsUri::fromLocalPath(QStringLiteral("/home/ann/100%3F.txt"));
    QCOMPARE(VfsUri::fromString(percent.toString()).fileName(), QStringLiteral("100%3F.txt"));
}

void TestVfsUri::aVersionBelongsToAFileAndNotToWhatIsAroundIt()
{
    const VfsUri earlier
        = VfsUri::fromString(QStringLiteral("mem:///docs/notes.txt")).withVersion(QStringLiteral("one"));

    // What is above a version of a file is the folder as it is now: a drive
    // issues versions of files, and nothing issued one of the directory.
    QVERIFY(!earlier.parent().hasVersion());
    QCOMPARE(earlier.parent().toString(), QStringLiteral("mem:///docs"));
    QVERIFY(!earlier.child(QStringLiteral("inner")).hasVersion());
}

/// Every uri ever written into a session, a bookmark, a set or the index was
/// written before this existed, and all of them still have to open.
void TestVfsUri::aUriWrittenBeforeVersionsExistedStillReadsTheSame()
{
    for (const QString& stored : { QStringLiteral("file:///home/ann/notes.txt"),
             QStringLiteral("s3://bucket/reports/q3.pdf"), QStringLiteral("file://server/share/a"),
             QStringLiteral("file:///C:/Users/ann"), QStringLiteral("mem:///") }) {
        const VfsUri parsed = VfsUri::fromString(stored);
        QVERIFY2(parsed.isValid(), qPrintable(stored));
        QVERIFY2(!parsed.hasVersion(), qPrintable(stored));
        QCOMPARE(parsed.toString(), stored);
    }
}

void TestVfsUri::localPathRoundTrip()
{
    const VfsUri uri = VfsUri::fromLocalPath(QStringLiteral("/home/user/notes.txt"));
    QCOMPARE(uri.scheme(), QStringLiteral("file"));
    QCOMPARE(uri.toLocalPath(), QStringLiteral("/home/user/notes.txt"));
}

void TestVfsUri::nonLocalHasNoLocalPath()
{
    QVERIFY(VfsUri::fromString(QStringLiteral("s3://bucket/key")).toLocalPath().isEmpty());
}

void TestVfsUri::equalityAndHashing()
{
    const VfsUri a = VfsUri::fromString(QStringLiteral("file:///home/user/"));
    const VfsUri b = VfsUri::fromString(QStringLiteral("file:///home/./user"));
    QCOMPARE(a, b);
    QCOMPARE(qHash(a), qHash(b));

    QVERIFY(a != VfsUri::fromString(QStringLiteral("file:///home/other")));
}

MOLE_TEST_MAIN(TestVfsUri)
#include "tst_VfsUri.moc"
