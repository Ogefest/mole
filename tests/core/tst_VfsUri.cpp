#include "support/MoleTestMain.h"

#include "core/vfs/VfsUri.h"

using namespace mole;

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
