#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/platform/Staging.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using namespace mole;
using namespace mole::test;

/// Where a payload goes while it is being written, and what happens when there is
/// nowhere to put it.
///
/// **The fault this suite exists for could not be asserted anywhere before it.**
/// The only way a test could arrange "no staging directory" was to point `TMPDIR`
/// at a path that does not exist and require the platform to refuse -- which is a
/// claim about Qt rather than about Mole, and it is not true: on one Qt build
/// `QDir::tempPath()` answers with nothing at all and `QTemporaryFile` then
/// creates its file in the filesystem root. That fails for an ordinary account
/// with `EACCES`, which is the only reason the old case ever passed, and succeeds
/// for root. So a download or an upload was staged where nobody would look for it
/// and sent from there. See MOLE-304, and MOLE-297 for how it surfaced.
///
/// The seam is `MOLE_STAGING_DIR`: a directory the test owns and can take away,
/// on any account and any platform, without touching `TMPDIR`.
class TestStaging : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void withNothingSetItStagesWhereTheSystemSays();
    void aFileIsCreatedInTheDirectoryThatWasAskedFor();
    void aDirectoryThatIsNotThereIsRefusedAndSaysWhich();
    void aDirectoryThatIsAFileIsRefused();
    void aScratchDirectoryIsMadeInsideItAndRefusedWhenItIsGone();
    void nothingStagesAnywhereElse();

    void aWriteThatGoesShortIsAFailureAndNotAFile();
    void aWriteThatCannotStartLeavesWhatWasThere();
    void aWholeWriteLandsUnderTheNameItWasAskedFor();

private:
    std::unique_ptr<QTemporaryDir> m_own;
    QByteArray m_previous;
};

namespace {

/// A device that takes some of what it is given and reports the rest as gone.
///
/// A full disk is not something a test can arrange, and it is the failure worth
/// asserting: a short write leaves a file that is the wrong length under the
/// right name, and everything downstream reads it as the file. So the seam is
/// the device rather than the filesystem. See MOLE-406.
class DeviceThatGoesShort final : public QIODevice
{
public:
    explicit DeviceThatGoesShort(qint64 accepts)
        : m_accepts(accepts)
    {
    }

    QByteArray taken() const { return m_taken; }

protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, qint64 size) override
    {
        const qint64 room = std::max<qint64>(0, m_accepts - m_taken.size());
        const qint64 taking = std::min(room, size);
        m_taken.append(data, static_cast<int>(taking));
        if (taking < size)
            setErrorString(QStringLiteral("no space left on device"));
        return taking;
    }

private:
    qint64 m_accepts = 0;
    QByteArray m_taken;
};

} // namespace

void TestStaging::init()
{
    m_previous = qgetenv("MOLE_STAGING_DIR");
    m_own = std::make_unique<QTemporaryDir>();
    QVERIFY(m_own->isValid());
}

void TestStaging::cleanup()
{
    if (m_previous.isEmpty())
        qunsetenv("MOLE_STAGING_DIR");
    else
        qputenv("MOLE_STAGING_DIR", m_previous);
    m_own.reset();
}

void TestStaging::withNothingSetItStagesWhereTheSystemSays()
{
    qunsetenv("MOLE_STAGING_DIR");
    QCOMPARE(staging::directory(), QDir::tempPath());

    QTemporaryFile file;
    QString why;
    QVERIFY2(staging::openFile(file, &why), qPrintable(why));
    QCOMPARE(QFileInfo(file.fileName()).absolutePath(), QFileInfo(QDir::tempPath()).absoluteFilePath());
}

void TestStaging::aFileIsCreatedInTheDirectoryThatWasAskedFor()
{
    // The half that is easy to lose: Qt decides where a temporary file goes when
    // it opens, from whatever the temporary path says at that moment. Asked for
    // here, so it cannot land anywhere else.
    qputenv("MOLE_STAGING_DIR", m_own->path().toUtf8());

    QTemporaryFile file;
    QString why;
    QVERIFY2(staging::openFile(file, &why), qPrintable(why));
    QVERIFY2(file.fileName().startsWith(m_own->path()), qPrintable(file.fileName()));
    QVERIFY(file.write(QByteArrayLiteral("a payload")) == 9);
    QVERIFY(file.flush());
    QVERIFY(QFileInfo::exists(file.fileName()));
}

void TestStaging::aDirectoryThatIsNotThereIsRefusedAndSaysWhich()
{
    const QString gone = QDir(m_own->path()).filePath(QStringLiteral("taken-away"));
    QVERIFY(QDir().mkpath(gone));
    qputenv("MOLE_STAGING_DIR", gone.toUtf8());

    // It works while the directory is there...
    {
        QTemporaryFile before;
        QString why;
        QVERIFY2(staging::openFile(before, &why), qPrintable(why));
    }

    // ...and is refused once it is not, on any account: there is no parent to
    // create anything in, and nothing is written anywhere else instead.
    QVERIFY(QDir(gone).removeRecursively());

    QTemporaryFile file;
    QString why;
    QVERIFY2(!staging::openFile(file, &why), qPrintable(file.fileName()));
    QVERIFY2(why.contains(gone), qPrintable(why));
    QVERIFY(file.fileName().isEmpty());

    // And nothing appeared in the filesystem root, which is where Qt would have
    // put it on the build this was found on.
    QVERIFY(!QFileInfo::exists(QStringLiteral("/mole-staging-")));
}

void TestStaging::aDirectoryThatIsAFileIsRefused()
{
    const QString notADirectory = QDir(m_own->path()).filePath(QStringLiteral("a-file"));
    QFile file(notADirectory);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArrayLiteral("not a directory"));
    file.close();
    qputenv("MOLE_STAGING_DIR", notADirectory.toUtf8());

    QTemporaryFile staged;
    QString why;
    QVERIFY(!staging::openFile(staged, &why));
    QVERIFY2(why.contains(QStringLiteral("not a directory")), qPrintable(why));
}

void TestStaging::aScratchDirectoryIsMadeInsideItAndRefusedWhenItIsGone()
{
    // The other half of what gets staged: a scratch directory, for an extracted
    // file, a rendered page or a table being imported.
    qputenv("MOLE_STAGING_DIR", m_own->path().toUtf8());

    QString why;
    std::unique_ptr<QTemporaryDir> scratch = staging::makeDirectory(&why);
    QVERIFY2(scratch, qPrintable(why));
    QVERIFY2(scratch->path().startsWith(m_own->path()), qPrintable(scratch->path()));

    const QString gone = QDir(m_own->path()).filePath(QStringLiteral("not-here"));
    qputenv("MOLE_STAGING_DIR", gone.toUtf8());
    QVERIFY(!staging::makeDirectory(&why));
    QVERIFY2(why.contains(gone), qPrintable(why));
}

void TestStaging::nothingStagesAnywhereElse()
{
    // One place, held by reading the source: eleven sites went through Qt
    // directly before this, and the eleventh is the one somebody would add next.
    // A scratch directory is made by staging::makeDirectory(), and a file that is
    // staged is opened by staging::openFile() -- so a source file that has a
    // QTemporaryFile in it and never mentions staging is one that opened its own.
    QStringList offenders;
    QDirIterator files(QStringLiteral(MOLE_SHELL_SOURCE_DIR),
        { QStringLiteral("*.cpp"), QStringLiteral("*.h") }, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        if (path.contains(QStringLiteral("platform/Staging.")))
            continue;
        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(source.readAll());
        const QString name = QFileInfo(path).fileName();

        for (const char* made : { "make_unique<QTemporaryDir>", "make_shared<QTemporaryDir>",
                 "new QTemporaryDir", "QTemporaryDir(" }) {
            if (text.contains(QLatin1String(made)))
                offenders.append(QStringLiteral("%1 makes its own scratch directory").arg(name));
        }
        if (path.endsWith(QStringLiteral(".cpp")) && text.contains(QStringLiteral("QTemporaryFile"))
            && !text.contains(QStringLiteral("staging::"))) {
            offenders.append(QStringLiteral("%1 stages a file without asking staging for it").arg(name));
        }
    }
    QVERIFY2(offenders.isEmpty(), qPrintable(offenders.join(QStringLiteral("; "))));
}

/// The fault, at the point where it can be produced.
void TestStaging::aWriteThatGoesShortIsAFailureAndNotAFile()
{
    const QByteArray payload(4096, 'x');
    DeviceThatGoesShort device(1000);
    QVERIFY(device.open(QIODevice::WriteOnly));

    const Result<void> written = staging::writeWholeTo(device, payload, QStringLiteral("report.pdf"));
    QVERIFY2(!written.ok(), "a write that took a thousand of four thousand bytes was called a success");
    QCOMPARE(written.error().code, VfsError::IoError);
    // The message says how much of it, because "could not write" and "wrote a
    // quarter of it" call for different things to be done next.
    QVERIFY2(written.error().message.contains(QStringLiteral("1000 of 4096")),
        qPrintable(written.error().message));
    QVERIFY2(
        written.error().message.contains(QStringLiteral("report.pdf")), qPrintable(written.error().message));
}

/// And a write that cannot start at all leaves whatever was at the name.
///
/// Through QSaveFile, so this is not merely "the open failed": a write that gets
/// part way and then cannot finish leaves the previous contents rather than the
/// wreckage of the new ones.
void TestStaging::aWriteThatCannotStartLeavesWhatWasThere()
{
    QTemporaryDir room;
    QVERIFY(room.isValid());
    const QString standing = QDir(room.path()).filePath(QStringLiteral("report.pdf"));
    {
        QFile file(standing);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArrayLiteral("the old one")), 11);
    }

    // A directory where the file should go: nothing can be written over one.
    const QString impossible = QDir(room.path()).filePath(QStringLiteral("folder"));
    QVERIFY(QDir().mkpath(impossible));
    const Result<void> refused = staging::writeWhole(impossible, QByteArrayLiteral("bytes"));
    QVERIFY2(!refused.ok(), "a write onto a directory was called a success");

    // And the ordinary name is untouched by any of it.
    QFile still(standing);
    QVERIFY(still.open(QIODevice::ReadOnly));
    QCOMPARE(still.readAll(), QByteArrayLiteral("the old one"));
}

void TestStaging::aWholeWriteLandsUnderTheNameItWasAskedFor()
{
    QTemporaryDir room;
    QVERIFY(room.isValid());
    const QString target = QDir(room.path()).filePath(QStringLiteral("extracted.bin"));

    QByteArray payload(200 * 1024, Qt::Uninitialized);
    for (int i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>(i & 0xff);

    QVERIFY(staging::writeWhole(target, payload).ok());

    QFile landed(target);
    QVERIFY(landed.open(QIODevice::ReadOnly));
    QCOMPARE(landed.readAll(), payload);
    // Nothing beside it: the working name is gone once the bytes are under the
    // real one.
    QCOMPARE(QDir(room.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}

MOLE_TEST_MAIN(TestStaging)

#include "tst_Staging.moc"
