#include "support/MoleTestMain.h"

#include "core/vfs/VfsUri.h"

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
