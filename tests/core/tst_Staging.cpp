#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/platform/Staging.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

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

private:
    std::unique_ptr<QTemporaryDir> m_own;
    QByteArray m_previous;
};

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

MOLE_TEST_MAIN(TestStaging)

#include "tst_Staging.moc"
