#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/data/JsonFileStore.h"

#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

using namespace mole;
using namespace mole::test;

namespace {

/// The smallest store there could be: one key, read and written.
class OneKeyStore final : public JsonFileStore
{
    Q_OBJECT

public:
    explicit OneKeyStore(const QString& path)
        : JsonFileStore(path)
    {
    }

    bool load()
    {
        QJsonObject root;
        const Read read = readRoot(&root);
        if (read == Read::Damaged)
            return false;
        m_value = read == Read::Missing ? QString() : root.value(QStringLiteral("value")).toString();
        return true;
    }

    [[nodiscard]] bool setValue(const QString& value)
    {
        m_value = value;
        return writeRoot(QJsonObject { { QStringLiteral("value"), value } });
    }

    QString value() const { return m_value; }

private:
    QString m_value;
};

} // namespace

/// What every store on disk does with a file, held once.
///
/// Ten of them repeated this and the same two faults were in all of them: a
/// write that did not land said nothing, and a file that could not be parsed was
/// replaced by an empty one. Held here rather than ten times over, which is the
/// point of there being one of these. See ADR-0089.
class TestJsonFileStore : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aFileThatIsNotThereIsNotAFailure();
    void whatWasWrittenComesBack();
    void aFileThatCannotBeParsedIsKeptBesideItself();
    void aSecondBadStartDoesNotOverwriteTheFirstCopy();
    void aWriteWithNowhereToGoSaysSoAndReportsItOnce();
    void theEnvironmentDecidesWhereAStoreLives();

private:
    QString path() const { return QDir(m_dir->path()).filePath(QStringLiteral("one.json")); }
    bool writeRaw(const QByteArray& bytes) const;

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestJsonFileStore::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

void TestJsonFileStore::cleanup()
{
    m_dir.reset();
}

bool TestJsonFileStore::writeRaw(const QByteArray& bytes) const
{
    QFile file(path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(bytes) == bytes.size();
}

void TestJsonFileStore::aFileThatIsNotThereIsNotAFailure()
{
    OneKeyStore store(path());
    QVERIFY2(store.load(), "a store with no file yet is a feature nobody has used");
    QVERIFY(!store.isDamaged());
    QVERIFY(store.value().isEmpty());
}

void TestJsonFileStore::whatWasWrittenComesBack()
{
    OneKeyStore store(path());
    QVERIFY(store.load());
    QVERIFY(store.setValue(QStringLiteral("kept")));

    OneKeyStore again(path());
    QVERIFY(again.load());
    QCOMPARE(again.value(), QStringLiteral("kept"));
}

/// The fault, in the smallest form it takes.
///
/// A store used to clear its model, fail to parse, return false and keep
/// nothing -- and the next write put the empty model over the file. One stray
/// byte was enough, and what it cost was everything somebody had configured.
void TestJsonFileStore::aFileThatCannotBeParsedIsKeptBesideItself()
{
    const QByteArray typedByHand("{ \"value\": \"mine\", }");
    QVERIFY(writeRaw(typedByHand));

    OneKeyStore store(path());
    QSignalSpy noticed(&store, &JsonFileStore::loadFoundDamage);

    QVERIFY2(!store.load(), "a file that could not be parsed is not a load that succeeded");
    QVERIFY(store.isDamaged());
    QCOMPARE(noticed.count(), 1);

    const QString kept = store.damagedCopyPath();
    QVERIFY2(!kept.isEmpty(), "the unreadable file has to be somewhere");
    QCOMPARE(noticed.first().first().toString(), kept);
    QFile keptFile(kept);
    QVERIFY(keptFile.open(QIODevice::ReadOnly));
    QCOMPARE(keptFile.readAll(), typedByHand);

    // Beside it, so what the store writes next goes where it always went and
    // costs nothing: the old file is safe, and refusing for the rest of the
    // session would leave the feature stuck with only a message to explain it.
    QVERIFY(store.setValue(QStringLiteral("new")));
    OneKeyStore again(path());
    QVERIFY(again.load());
    QCOMPARE(again.value(), QStringLiteral("new"));
}

void TestJsonFileStore::aSecondBadStartDoesNotOverwriteTheFirstCopy()
{
    QVERIFY(writeRaw(QByteArray("first")));
    OneKeyStore first(path());
    QVERIFY(!first.load());
    const QString oldest = first.damagedCopyPath();
    QVERIFY(!oldest.isEmpty());

    QVERIFY(writeRaw(QByteArray("second")));
    OneKeyStore second(path());
    QVERIFY(!second.load());
    QVERIFY2(second.damagedCopyPath() != oldest,
        "a second bad start overwrote the copy the first one kept, which is this fault again");

    QFile keptFirst(oldest);
    QVERIFY(keptFirst.open(QIODevice::ReadOnly));
    QCOMPARE(keptFirst.readAll(), QByteArray("first"));
}

void TestJsonFileStore::aWriteWithNowhereToGoSaysSoAndReportsItOnce()
{
#ifndef Q_OS_UNIX
    QSKIP("permissions work differently on this platform");
#else
    if (geteuid() == 0)
        QSKIP("running as root, where a read-only directory is not read-only");

    const QString folder = QDir(m_dir->path()).filePath(QStringLiteral("locked"));
    QVERIFY(QDir().mkpath(folder));
    OneKeyStore store(QDir(folder).filePath(QStringLiteral("one.json")));
    QVERIFY(store.load());

    if (!madeUnreadable(folder))
        QSKIP("this account can write into a directory with no permissions at all");

    QSignalSpy complained(&store, &JsonFileStore::saveFailed);
    CapturedWarnings logged;
    const bool written = store.setValue(QStringLiteral("nowhere"));
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QVERIFY2(!written, "a write that did not land was reported as one that did");
    QCOMPARE(complained.count(), 1);
    // The reason names the file and nothing else: this goes on a screen and
    // into the session log, and neither may carry a directory off somebody's
    // machine. See ADR-0064.
    const QString reason = complained.first().first().toString();
    QVERIFY2(reason.contains(QStringLiteral("one.json")), qPrintable(reason));
    QVERIFY2(!reason.contains(folder), qPrintable(reason));
    QVERIFY2(logged.contains(QStringLiteral("one.json")), qPrintable(logged.joined()));
#endif
}

void TestJsonFileStore::theEnvironmentDecidesWhereAStoreLives()
{
    const QString chosen = QDir(m_dir->path()).filePath(QStringLiteral("elsewhere.json"));
    qputenv("MOLE_TEST_STORE_PATH", chosen.toLocal8Bit());
    QCOMPARE(JsonFile::pathFor("MOLE_TEST_STORE_PATH", QStringLiteral("one.json")), chosen);

    qunsetenv("MOLE_TEST_STORE_PATH");
    const QString fallback = JsonFile::pathFor("MOLE_TEST_STORE_PATH", QStringLiteral("one.json"));
    QVERIFY2(fallback.endsWith(QStringLiteral("one.json")), qPrintable(fallback));
    QVERIFY(fallback != chosen);
}

MOLE_TEST_MAIN(TestJsonFileStore)
#include "tst_JsonFileStore.moc"
