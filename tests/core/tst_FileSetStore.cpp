#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/sets/FileSetStore.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace mole;
using namespace mole::test;

/// The sets file, and what happens to it when the disk will not cooperate.
///
/// A set is a list somebody assembled by hand out of a search, a duplicate scan
/// or a folder -- work, not a cache. The two ways it used to be lost are the two
/// this file holds: a write that did not land and said nothing, and a file that
/// could not be parsed being replaced by an empty one. See ADR-0089.
class TestFileSetStore : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void whatWasCreatedComesBack();
    void aSetsFileThatCannotBeParsedIsKeptRatherThanReplaced();
    void aSetThatCannotBeWrittenSaysSo();

private:
    QString path() const { return QDir(m_dir->path()).filePath(QStringLiteral("sets.json")); }

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestFileSetStore::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

void TestFileSetStore::cleanup()
{
    m_dir.reset();
}

void TestFileSetStore::whatWasCreatedComesBack()
{
    FileSetStore store(path());
    QVERIFY(store.load());
    const FileSet made = store.create(QStringLiteral("Invoices"), { QStringLiteral("file:///a.pdf") });
    QVERIFY(made.isValid());

    FileSetStore reopened(path());
    QVERIFY(reopened.load());
    QCOMPARE(reopened.sets().size(), 1);
    QCOMPARE(reopened.sets().first().name, QStringLiteral("Invoices"));
}

void TestFileSetStore::aSetsFileThatCannotBeParsedIsKeptRatherThanReplaced()
{
    const QByteArray typedByHand("{ \"sets\": [ { \"name\": \"Invoices\", } ] }");
    {
        QFile file(path());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(typedByHand);
    }

    FileSetStore store(path());
    QVERIFY2(!store.load(), "a file that could not be parsed is not a load that succeeded");

    const QString kept = store.damagedCopyPath();
    QVERIFY2(!kept.isEmpty(), "the unreadable file has to be somewhere");
    QFile keptFile(kept);
    QVERIFY(keptFile.open(QIODevice::ReadOnly));
    QCOMPARE(keptFile.readAll(), typedByHand);

    QVERIFY(store.create(QStringLiteral("Later"), { QStringLiteral("file:///b.pdf") }).isValid());
    FileSetStore reopened(path());
    QVERIFY(reopened.load());
    QCOMPARE(reopened.sets().size(), 1);
}

void TestFileSetStore::aSetThatCannotBeWrittenSaysSo()
{
#ifndef Q_OS_UNIX
    QSKIP("permissions work differently on this platform");
#else
    if (geteuid() == 0)
        QSKIP("running as root, where a read-only directory is not read-only");

    const QString folder = QDir(m_dir->path()).filePath(QStringLiteral("locked"));
    QVERIFY(QDir().mkpath(folder));
    FileSetStore store(QDir(folder).filePath(QStringLiteral("sets.json")));
    QVERIFY(store.load());

    if (!madeUnreadable(folder))
        QSKIP("this account can write into a directory with no permissions at all");

    QSignalSpy complained(&store, &JsonFileStore::saveFailed);
    FileSet set;
    set.id = QStringLiteral("one");
    set.name = QStringLiteral("Invoices");
    set.uris = { QStringLiteral("file:///a.pdf") };
    const bool stored = store.put(set);
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QVERIFY2(!stored, "a set that could not be written was reported as stored");
    QCOMPARE(complained.count(), 1);
    QVERIFY2(complained.first().first().toString().contains(QStringLiteral("sets.json")),
        qPrintable(complained.first().first().toString()));
#endif
}

MOLE_TEST_MAIN(TestFileSetStore)
#include "tst_FileSetStore.moc"
