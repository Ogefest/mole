#include "support/MoleTestMain.h"

#include "core/vfs/DiskIdentity.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace mole;

/// What a disk is, read from links to its device, against a tree this test makes
/// instead of this machine's /dev/disk. See MOLE-430.
class TestDiskIdentity : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void aDeviceIsKnownByItsUuidAndItsLabel();
    void aLabelIsDecodedTheWayUdevEscapesIt_data();
    void aLabelIsDecodedTheWayUdevEscapesIt();
    void aDeviceNothingLinksToHasNoIdentity();
    void somethingThatIsNotADeviceHasNoIdentity();
    void aMachineWithNoDiskDirectoryHasNoIdentities();

private:
    QString device(const QString& name);
    void link(const QString& kind, const QString& name, const QString& target);

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestDiskIdentity::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("dev")));
    QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("disk/by-uuid")));
    QVERIFY(QDir(m_dir->path()).mkpath(QStringLiteral("disk/by-label")));
}

QString TestDiskIdentity::device(const QString& name)
{
    const QString path = m_dir->filePath(QStringLiteral("dev/") + name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    return path;
}

void TestDiskIdentity::link(const QString& kind, const QString& name, const QString& target)
{
    // Relative, as udev makes them: ../../sdb1.
    const QString at = m_dir->filePath(QStringLiteral("disk/") + kind + QLatin1Char('/') + name);
    QVERIFY(QFile::link(QStringLiteral("../../dev/") + QFileInfo(target).fileName(), at));
}

void TestDiskIdentity::aDeviceIsKnownByItsUuidAndItsLabel()
{
    const QString stick = device(QStringLiteral("sdb1"));
    const QString other = device(QStringLiteral("sdc1"));
    link(QStringLiteral("by-uuid"), QStringLiteral("0A1B-2C3D"), stick);
    link(QStringLiteral("by-label"), QStringLiteral("BACKUP"), stick);
    link(QStringLiteral("by-uuid"), QStringLiteral("5e6f7a8b-0000-4c4c-9a9a-123456789abc"), other);

    const DiskIdentity identity = diskIdentityOf(stick, m_dir->filePath(QStringLiteral("disk")));
    QCOMPARE(identity.uuid, QStringLiteral("0A1B-2C3D"));
    QCOMPARE(identity.label, QStringLiteral("BACKUP"));

    // The other disk is its own, and carries no label.
    const DiskIdentity second = diskIdentityOf(other, m_dir->filePath(QStringLiteral("disk")));
    QCOMPARE(second.uuid, QStringLiteral("5e6f7a8b-0000-4c4c-9a9a-123456789abc"));
    QVERIFY(second.label.isEmpty());
}

void TestDiskIdentity::aLabelIsDecodedTheWayUdevEscapesIt_data()
{
    QTest::addColumn<QString>("linkName");
    QTest::addColumn<QString>("label");
    QTest::newRow("plain") << "BACKUP" << "BACKUP";
    QTest::newRow("a space") << "MY\\x20STICK" << "MY STICK";
    QTest::newRow("a slash") << "a\\x2fb" << "a/b";
    QTest::newRow("utf-8, byte by byte") << "Zdj\\xc4\\x99cia" << "Zdjęcia";
    QTest::newRow("a backslash with nothing after it") << "odd\\x" << "odd\\x";
}

void TestDiskIdentity::aLabelIsDecodedTheWayUdevEscapesIt()
{
    QFETCH(QString, linkName);
    QFETCH(QString, label);
    QCOMPARE(decodeDiskLabel(linkName), label);
}

void TestDiskIdentity::aDeviceNothingLinksToHasNoIdentity()
{
    const QString stick = device(QStringLiteral("sdb1"));
    const DiskIdentity identity = diskIdentityOf(stick, m_dir->filePath(QStringLiteral("disk")));
    QVERIFY(identity.uuid.isEmpty());
    QVERIFY(identity.label.isEmpty());
}

void TestDiskIdentity::somethingThatIsNotADeviceHasNoIdentity()
{
    // What the mount table names for a ZFS dataset, an NFS export and a tmpfs.
    for (const QString& notADevice :
        { QStringLiteral("rpool/home"), QStringLiteral("server:/export"), QStringLiteral("tmpfs") })
        QVERIFY(diskIdentityOf(notADevice, m_dir->filePath(QStringLiteral("disk"))).uuid.isEmpty());
}

void TestDiskIdentity::aMachineWithNoDiskDirectoryHasNoIdentities()
{
    // macOS and Windows have no /dev/disk of this shape.
    const QString stick = device(QStringLiteral("sdb1"));
    QVERIFY(diskIdentityOf(stick, m_dir->filePath(QStringLiteral("nowhere"))).uuid.isEmpty());
}

MOLE_TEST_MAIN(TestDiskIdentity)
#include "tst_DiskIdentity.moc"
