#include "support/FaultyFileSystem.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "tools/tasks/Commands.h"
#include "tools/tasks/ToolEnvironment.h"

#include "core/credentials/SecretStore.h"
#include "core/index/IndexDatabase.h"
#include "core/search/SearchQuery.h"
#include "core/vfs/RemoteRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>

using namespace mole;
using namespace mole::test;
using namespace mole::tools;

namespace {

/// One run of the console runner, with everything it printed.
struct Run
{
    int code = 0;
    QString out;
    QString err;

    bool said(const QString& needle) const { return out.contains(needle) || err.contains(needle); }
    QString both() const { return out + err; }
};

} // namespace

/// The console runner, driven in-process.
///
/// In-process rather than by starting the binary: a runner tested only through
/// a process gives up its own failure messages -- what a test can assert on is
/// an exit code and a blob of text -- and the exit codes are half of what this
/// tool is for. The binary itself is four lines around runMoleTasks(), and there
/// is one test at the end that runs it for real, because "with no window" is a
/// claim about a process and cannot be checked from inside one.
class TestMoleTasks : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void noArgumentsPrintsTheUsageAndSaysSo();
    void anUnknownCommandIsAUsageError();
    void anOptionWithNoValueIsAUsageError();
    void aStrayOptionIsNotIgnored();

    void copiesBetweenLocalDirectories();
    void copyOntoAnExistingNameFailsAndNamesTheFile();
    void anUnmountedSchemeSaysWhatIsMounted();
    void movesAndDeletes();

    void syncSaysWhatItWouldDoAndDoesNothing();
    void syncAppliesWhenAsked();
    void renamePreviewsThenApplies();
    void duplicatesReportsWhatCouldBeFreed();
    void compressPacksWhatItWasGiven();
    void verifyAnswersForADriveThatIsThere();

    void aMountSpecCarriesItsSecretInTheEnvironment();
    void aMountSpecWithoutANameIsRefused();
    void anUnknownDriveTypeSaysTheDriveCouldNotBeReached();
    void theBinaryRunsWithNoDisplay();

    // ---- reaching a drive the way the window reaches it ----
    void aRootedDriveIsMountedWhereTheWindowMountsIt();

    // ---- what a person types, read strictly ----
    void helpAndVersionAreAnAnswerRatherThanAMistake();
    void anOptionBeforeTheCommandIsNotTheCommand();
    void aStrayOptionIsMentionedEvenWhenTheRunFailed();
    void aValueThatIsNotOneOfTheAcceptedOnesIsRefused_data();
    void aValueThatIsNotOneOfTheAcceptedOnesIsRefused();
    void aPassphraseIsNamedRatherThanTyped();

    // ---- the result and everything else ----
    void progressGoesToStandardErrorAndTheResultToStandardOutput();
    void anInterruptedRunSaysSoAndExitsOneThirty();

    // ---- as complete a scan as the window builds ----
    void aScanRecordsWhatIsInsideAContainer();
    void drivesCanSayWhetherThePluginsLoaded();

private:
    Run run(const QStringList& arguments);
    QString uriFor(const QString& relative) const;
    bool write(const QString& relative, const QByteArray& contents = "payload");
    QStringList entriesIn(const QString& relative) const;
    /// Writes a drive into the configuration file the runner reads, the way the
    /// window's own dialog writes one.
    bool seedDrive(const QString& name, const QString& factoryScheme, const QString& root);
    /// Replaces the local mount with the same drive behind a fault injector, so
    /// a case can hold a transfer still without a clock anywhere.
    std::shared_ptr<FaultyFileSystem> makeLocalDriveFaulty();

    std::unique_ptr<PrivateProfile> m_profile;
    std::unique_ptr<TempTree> m_tree;
    std::unique_ptr<ToolEnvironment> m_environment;
};

void TestMoleTasks::init()
{
    // The runner reads the drives, the credentials and the index the
    // application uses. A test that ran against the real ones would scan
    // somebody's home directory into their own index.
    m_profile = std::make_unique<PrivateProfile>();
    QVERIFY(m_profile->isValid());
    m_tree = std::make_unique<TempTree>();
    QVERIFY(m_tree->isValid());
    m_environment = std::make_unique<ToolEnvironment>();
}

void TestMoleTasks::cleanup()
{
    m_environment.reset();
    m_tree.reset();
    m_profile.reset();
}

Run TestMoleTasks::run(const QStringList& arguments)
{
    Run result;
    QTextStream out(&result.out);
    QTextStream err(&result.err);
    result.code = runMoleTasks(arguments, *m_environment, out, err);
    out.flush();
    err.flush();
    return result;
}

QString TestMoleTasks::uriFor(const QString& relative) const
{
    return VfsUri::fromLocalPath(m_tree->absolute(relative)).toString();
}

bool TestMoleTasks::write(const QString& relative, const QByteArray& contents)
{
    return m_tree->writeFile(relative, contents);
}

QStringList TestMoleTasks::entriesIn(const QString& relative) const
{
    return QDir(m_tree->absolute(relative)).entryList(QDir::Files | QDir::Hidden, QDir::Name);
}

bool TestMoleTasks::seedDrive(const QString& name, const QString& factoryScheme, const QString& root)
{
    SecretStore secrets(SecretStore::defaultPath());
    RemoteRegistry registry(RemoteRegistry::defaultPath(), &secrets);
    registry.load();

    RemoteDrive drive;
    drive.id = QStringLiteral("seeded-") + name.toLower();
    drive.name = name;
    drive.factoryScheme = factoryScheme;
    drive.root = root;
    return registry.put(drive, {});
}

std::shared_ptr<FaultyFileSystem> TestMoleTasks::makeLocalDriveFaulty()
{
    const VfsUri root = VfsUri(QStringLiteral("file"), QString(), QStringLiteral("/"));
    FileSystemPtr disk = m_environment->drives().resolve(root);
    if (!disk)
        return nullptr;

    auto faulty = std::make_shared<FaultyFileSystem>(disk);
    m_environment->drives().removeMount(QStringLiteral("local"));
    Mount mount;
    mount.id = QStringLiteral("local");
    mount.displayName = QStringLiteral("Local disk");
    mount.root = root;
    mount.fileSystem = faulty;
    m_environment->drives().addMount(std::move(mount));
    return faulty;
}

void TestMoleTasks::noArgumentsPrintsTheUsageAndSaysSo()
{
    const Run result = run({});
    QCOMPARE(result.code, BadUsage);
    // On stderr: stdout is the result, and a run that did nothing has none.
    QVERIFY2(result.err.contains(QStringLiteral("mole-tasks <command>")), qPrintable(result.both()));
    QVERIFY2(result.out.isEmpty(), qPrintable(result.out));
}

void TestMoleTasks::anUnknownCommandIsAUsageError()
{
    const Run result = run({ QStringLiteral("frobnicate") });
    QCOMPARE(result.code, BadUsage);
    QVERIFY2(result.err.contains(QStringLiteral("no such command")), qPrintable(result.both()));
}

void TestMoleTasks::anOptionWithNoValueIsAUsageError()
{
    // `--to` with nothing after it used to leave the target empty, which read
    // as "you forgot --to" -- a different mistake with a different fix.
    const Run result = run({ QStringLiteral("copy"), QStringLiteral("--from"), uriFor(QStringLiteral("a")),
        QStringLiteral("--to") });
    QCOMPARE(result.code, BadUsage);
    QVERIFY2(result.err.contains(QStringLiteral("needs a value")), qPrintable(result.both()));
}

void TestMoleTasks::aStrayOptionIsNotIgnored()
{
    QVERIFY(write(QStringLiteral("src/note.txt")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("dst")));

    const Run result
        = run({ QStringLiteral("copy"), QStringLiteral("--from"), uriFor(QStringLiteral("src/note.txt")),
            QStringLiteral("--to"), uriFor(QStringLiteral("dst")), QStringLiteral("--recursive") });

    // The copy happened; what must not happen is the run being called a
    // success while an option nobody read was on the command line.
    QCOMPARE(result.code, BadUsage);
    QVERIFY2(result.err.contains(QStringLiteral("--recursive")), qPrintable(result.both()));
}

void TestMoleTasks::copiesBetweenLocalDirectories()
{
    QVERIFY(write(QStringLiteral("src/note.txt"), QByteArray("hello")));
    QVERIFY(write(QStringLiteral("src/other.bin"), QByteArray(4096, 'x')));
    QVERIFY(m_tree->makeDirs(QStringLiteral("dst")));

    const Run result = run({ QStringLiteral("copy"), QStringLiteral("--from"),
        uriFor(QStringLiteral("src/note.txt")), QStringLiteral("--from"),
        uriFor(QStringLiteral("src/other.bin")), QStringLiteral("--to"), uriFor(QStringLiteral("dst")) });

    QCOMPARE(result.code, Ok);
    QCOMPARE(entriesIn(QStringLiteral("dst")),
        QStringList({ QStringLiteral("note.txt"), QStringLiteral("other.bin") }));
    QVERIFY2(result.out.contains(QStringLiteral("2 transferred")), qPrintable(result.both()));
}

void TestMoleTasks::copyOntoAnExistingNameFailsAndNamesTheFile()
{
    QVERIFY(write(QStringLiteral("src/note.txt"), QByteArray("new")));
    QVERIFY(write(QStringLiteral("dst/note.txt"), QByteArray("old")));

    const Run result = run({ QStringLiteral("copy"), QStringLiteral("--from"),
        uriFor(QStringLiteral("src/note.txt")), QStringLiteral("--to"), uriFor(QStringLiteral("dst")) });

    // A shell script driving this has to be able to tell a copy that did not
    // happen from one that did.
    QCOMPARE(result.code, TaskFailed);
    QVERIFY2(result.err.contains(QStringLiteral("note.txt")), qPrintable(result.both()));

    QFile kept(m_tree->absolute(QStringLiteral("dst/note.txt")));
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), QByteArray("old"));
}

void TestMoleTasks::anUnmountedSchemeSaysWhatIsMounted()
{
    QVERIFY(m_tree->makeDirs(QStringLiteral("dst")));

    const Run result = run(
        { QStringLiteral("copy"), QStringLiteral("--from"), QStringLiteral("sftp:///somewhere/file.bin"),
            QStringLiteral("--to"), uriFor(QStringLiteral("dst")) });

    // Its own exit code: "the drive was never there" and "the copy failed" ask
    // for different things from whoever is watching.
    QCOMPARE(result.code, NoDrive);
    QVERIFY2(result.err.contains(QStringLiteral("no drive is mounted")), qPrintable(result.both()));
    QVERIFY2(result.err.contains(QStringLiteral("file:///")), qPrintable(result.both()));
}

void TestMoleTasks::movesAndDeletes()
{
    QVERIFY(write(QStringLiteral("src/note.txt"), QByteArray("hello")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("dst")));

    const Run moved = run({ QStringLiteral("move"), QStringLiteral("--from"),
        uriFor(QStringLiteral("src/note.txt")), QStringLiteral("--to"), uriFor(QStringLiteral("dst")) });
    QCOMPARE(moved.code, Ok);
    QVERIFY(entriesIn(QStringLiteral("src")).isEmpty());
    QCOMPARE(entriesIn(QStringLiteral("dst")), QStringList { QStringLiteral("note.txt") });

    const Run deleted = run({ QStringLiteral("delete"), uriFor(QStringLiteral("dst/note.txt")) });
    QCOMPARE(deleted.code, Ok);
    QVERIFY(entriesIn(QStringLiteral("dst")).isEmpty());
}

void TestMoleTasks::syncSaysWhatItWouldDoAndDoesNothing()
{
    QVERIFY(write(QStringLiteral("a/one.txt")));
    QVERIFY(write(QStringLiteral("a/two.txt")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("b")));

    const Run result = run({ QStringLiteral("sync"), QStringLiteral("--from"), uriFor(QStringLiteral("a")),
        QStringLiteral("--to"), uriFor(QStringLiteral("b")) });

    QCOMPARE(result.code, Ok);
    QVERIFY2(result.out.contains(QStringLiteral("dry run")), qPrintable(result.both()));
    QVERIFY2(result.out.contains(QStringLiteral("one.txt")), qPrintable(result.both()));
    // The point of a dry run being the default: nothing moved.
    QVERIFY(entriesIn(QStringLiteral("b")).isEmpty());
}

void TestMoleTasks::syncAppliesWhenAsked()
{
    QVERIFY(write(QStringLiteral("a/one.txt")));
    QVERIFY(write(QStringLiteral("a/two.txt")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("b")));

    const Run result = run({ QStringLiteral("sync"), QStringLiteral("--from"), uriFor(QStringLiteral("a")),
        QStringLiteral("--to"), uriFor(QStringLiteral("b")), QStringLiteral("--apply") });

    QCOMPARE(result.code, Ok);
    QCOMPARE(entriesIn(QStringLiteral("b")),
        QStringList({ QStringLiteral("one.txt"), QStringLiteral("two.txt") }));
}

void TestMoleTasks::renamePreviewsThenApplies()
{
    QVERIFY(write(QStringLiteral("shots/IMG_1.jpg")));
    QVERIFY(write(QStringLiteral("shots/IMG_2.jpg")));

    const Run preview = run({ QStringLiteral("rename"), QStringLiteral("--in"),
        uriFor(QStringLiteral("shots")), QStringLiteral("--find"), QStringLiteral("IMG"),
        QStringLiteral("--replace"), QStringLiteral("Photo") });
    QCOMPARE(preview.code, Ok);
    QVERIFY2(preview.out.contains(QStringLiteral("IMG_1.jpg  ->  Photo_1.jpg")), qPrintable(preview.both()));
    QCOMPARE(entriesIn(QStringLiteral("shots")),
        QStringList({ QStringLiteral("IMG_1.jpg"), QStringLiteral("IMG_2.jpg") }));

    const Run applied = run({ QStringLiteral("rename"), QStringLiteral("--in"),
        uriFor(QStringLiteral("shots")), QStringLiteral("--find"), QStringLiteral("IMG"),
        QStringLiteral("--replace"), QStringLiteral("Photo"), QStringLiteral("--apply") });
    QCOMPARE(applied.code, Ok);
    QCOMPARE(entriesIn(QStringLiteral("shots")),
        QStringList({ QStringLiteral("Photo_1.jpg"), QStringLiteral("Photo_2.jpg") }));
}

void TestMoleTasks::duplicatesReportsWhatCouldBeFreed()
{
    QVERIFY(write(QStringLiteral("tree/one.txt"), QByteArray("the same thing")));
    QVERIFY(write(QStringLiteral("tree/deep/two.txt"), QByteArray("the same thing")));
    QVERIFY(write(QStringLiteral("tree/three.txt"), QByteArray("something else")));

    const Run result = run({ QStringLiteral("duplicates"), uriFor(QStringLiteral("tree")),
        QStringLiteral("--by"), QStringLiteral("content") });

    QCOMPARE(result.code, Ok);
    QVERIFY2(result.out.contains(QStringLiteral("1 group(s)")), qPrintable(result.both()));
    QVERIFY2(result.out.contains(QStringLiteral("one.txt")), qPrintable(result.both()));
    QVERIFY2(result.out.contains(QStringLiteral("two.txt")), qPrintable(result.both()));
    QVERIFY2(!result.out.contains(QStringLiteral("three.txt")), qPrintable(result.both()));
}

/// The command the released builds could not run.
///
/// `compress` is compiled behind MOLE_HAVE_ARCHIVE, which src/tools/CMakeLists.txt
/// sets from MOLE_HAVE_ARCHIVE_PLUGIN -- a cache variable src/plugins writes. On a
/// *fresh* configure, src/tools used to be processed first, so the variable was
/// undefined and the runner was compiled without it: every CI job, every
/// packaging container and `make deb` shipped a mole-tasks whose compress said
/// "this build has no archive support (libarchive was not found)". On the second
/// configure of the same directory the cached value was ON and it worked, so a
/// developer's tree could pack and no artefact could.
///
/// Nothing noticed because tests/ is configured after src/plugins -- the suite
/// links the writer whatever the runner does -- and no case ran the command. This
/// is that case. See MOLE-386.
void TestMoleTasks::compressPacksWhatItWasGiven()
{
#ifndef MOLE_HAVE_ARCHIVE
    QSKIP("this build was made without libarchive");
#else
    QVERIFY(write(QStringLiteral("src/one.txt"), QByteArray("the first file")));
    QVERIFY(write(QStringLiteral("src/two.txt"), QByteArray("the second file")));

    const Run result
        = run({ QStringLiteral("compress"), QStringLiteral("--from"), uriFor(QStringLiteral("src/one.txt")),
            QStringLiteral("--from"), uriFor(QStringLiteral("src/two.txt")), QStringLiteral("--to"),
            uriFor(QStringLiteral("bundle.zip")) });

    QCOMPARE(result.code, Ok);
    QVERIFY2(result.out.contains(QStringLiteral("2 packed")), qPrintable(result.both()));

    // The archive is there and holds both files. Read with unzip rather than
    // through Mole's own archive backend: what is in question is what was
    // written, and an outside reader is the better witness for that.
    const QString archive = m_tree->absolute(QStringLiteral("bundle.zip"));
    QVERIFY2(QFileInfo(archive).size() > 0, "the archive was never written");

    const QString unzip = QStandardPaths::findExecutable(QStringLiteral("unzip"));
    if (unzip.isEmpty())
        QSKIP("unzip is not available to read the archive back");

    QProcess reader;
    reader.start(unzip, { QStringLiteral("-Z1"), archive });
    QVERIFY(reader.waitForFinished(30000));
    QStringList packed
        = QString::fromUtf8(reader.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    packed.sort();
    QCOMPARE(packed, QStringList({ QStringLiteral("one.txt"), QStringLiteral("two.txt") }));
#endif
}

void TestMoleTasks::verifyAnswersForADriveThatIsThere()
{
    QVERIFY(write(QStringLiteral("live/one.txt")));

    const Run result = run({ QStringLiteral("verify"), uriFor(QStringLiteral("live")) });
    QCOMPARE(result.code, Ok);
    QVERIFY2(result.out.contains(QStringLiteral("Connected")), qPrintable(result.both()));
}

void TestMoleTasks::aMountSpecCarriesItsSecretInTheEnvironment()
{
    qputenv("MOLE_TEST_SPEC_SECRET", "opensesame");

    const MountSpec spec = parseMountSpec(
        QStringLiteral("name=NAS box,type=sftp,host=example,password=@MOLE_TEST_SPEC_SECRET,root=/data"));

    QVERIFY2(spec.isValid(), qPrintable(spec.problem));
    QCOMPARE(spec.name, QStringLiteral("NAS box"));
    QCOMPARE(spec.type, QStringLiteral("sftp"));
    QCOMPARE(spec.root, QStringLiteral("/data"));
    QCOMPARE(spec.config.value(QStringLiteral("host")).toString(), QStringLiteral("example"));
    // The whole point: the password was never an argument.
    QCOMPARE(spec.config.value(QStringLiteral("password")).toString(), QStringLiteral("opensesame"));

    qunsetenv("MOLE_TEST_SPEC_SECRET");
}

void TestMoleTasks::aMountSpecWithoutANameIsRefused()
{
    QVERIFY(!parseMountSpec(QStringLiteral("type=sftp,host=example")).isValid());
    QVERIFY(!parseMountSpec(QStringLiteral("name=x")).isValid());
    QVERIFY(!parseMountSpec(QStringLiteral("name=x,type=sftp,nonsense")).isValid());
}

void TestMoleTasks::anUnknownDriveTypeSaysTheDriveCouldNotBeReached()
{
    const Run result
        = run({ QStringLiteral("drives"), QStringLiteral("--mount"), QStringLiteral("name=x,type=nosuch") });

    QCOMPARE(result.code, NoDrive);
    QVERIFY2(result.err.contains(QStringLiteral("Nothing here can serve")), qPrintable(result.both()));
}

// ------------------------ reaching a drive the way the window reaches it

void TestMoleTasks::aRootedDriveIsMountedWhereTheWindowMountsIt()
{
    // A drive rooted inside the remote, which is the ordinary shape of one: the
    // window mounts it at nas://NAS/ and lets the backend prefix /data, because
    // the root travels in the configuration as __root. The runner applied it in
    // both places, so nothing under nas://NAS/ resolved and anything that did
    // reached /data/data/…. A drive rooted at / behaves the same either way,
    // which is why every existing case here passed.
    QVERIFY(seedDrive(QStringLiteral("NAS"), QStringLiteral("mem"), QStringLiteral("/data")));

    const QString windowUri
        = RemoteDrive { {}, QStringLiteral("NAS"), {}, {}, QStringLiteral("/data"), {}, {}, true }
              .rootUri()
              .toString();

    const Run mounted = run({ QStringLiteral("--drive"), QStringLiteral("NAS"), QStringLiteral("drives") });
    QCOMPARE(mounted.code, Ok);
    QVERIFY2(mounted.out.contains(windowUri + QStringLiteral("  NAS")), qPrintable(mounted.both()));

    // And the uri a bookmark or a session carries actually resolves.
    QVERIFY(write(QStringLiteral("a.txt")));
    const Run copied = run({ QStringLiteral("--drive"), QStringLiteral("NAS"), QStringLiteral("copy"),
        QStringLiteral("--from"), uriFor(QStringLiteral("a.txt")), QStringLiteral("--to"), windowUri });
    QCOMPARE(copied.code, Ok);
}

// ------------------------------------ what a person types, read strictly

void TestMoleTasks::helpAndVersionAreAnAnswerRatherThanAMistake()
{
    // Both used to exit 2. --version was an unknown option, fell through to "no
    // command" and printed the usage -- on the binary whose whole purpose is a
    // machine with no display, where "which build is this" is the first question
    // of every report.
    const Run help = run({ QStringLiteral("--help") });
    QCOMPARE(help.code, Ok);
    QVERIFY2(help.out.contains(QStringLiteral("mole-tasks <command>")), qPrintable(help.both()));

    const Run version = run({ QStringLiteral("--version") });
    QCOMPARE(version.code, Ok);
    QVERIFY2(version.out.contains(QCoreApplication::applicationVersion()), qPrintable(version.both()));

    // The bare word kept working, and still prints to stdout.
    const Run word = run({ QStringLiteral("help") });
    QCOMPARE(word.code, Ok);
    QVERIFY2(word.out.contains(QStringLiteral("mole-tasks <command>")), qPrintable(word.both()));
}

void TestMoleTasks::anOptionBeforeTheCommandIsNotTheCommand()
{
    // The value of the misplaced option used to become the command word, so the
    // message named a path: "no such command: /x".
    QVERIFY(write(QStringLiteral("a.txt")));
    const Run result = run({ QStringLiteral("--to"), uriFor(QString()), QStringLiteral("copy"),
        QStringLiteral("--from"), uriFor(QStringLiteral("a.txt")) });

    QCOMPARE(result.code, BadUsage);
    QVERIFY2(result.err.contains(QStringLiteral("comes after the command word")), qPrintable(result.both()));
    QVERIFY2(!result.err.contains(QStringLiteral("no such command")), qPrintable(result.both()));
}

void TestMoleTasks::aStrayOptionIsMentionedEvenWhenTheRunFailed()
{
    // The case where a typo is the likeliest explanation was the one case that
    // said nothing: the stray was reported only on a clean run.
    const Run result = run(
        { QStringLiteral("delete"), uriFor(QStringLiteral("gone.txt")), QStringLiteral("--recursive") });

    QVERIFY2(result.err.contains(QStringLiteral("--recursive means nothing to delete")),
        qPrintable(result.both()));
    QCOMPARE(result.code, TaskFailed); // the delete's own answer still wins
}

void TestMoleTasks::aValueThatIsNotOneOfTheAcceptedOnesIsRefused_data()
{
    QTest::addColumn<QStringList>("arguments");
    QTest::addColumn<QString>("says");

    QTest::newRow("--mode") << QStringList { QStringLiteral("sync"), QStringLiteral("--from"),
        QStringLiteral("mem:///a"), QStringLiteral("--to"), QStringLiteral("mem:///b"),
        QStringLiteral("--mode"), QStringLiteral("miror"), QStringLiteral("--apply") }
                            << QStringLiteral("mirror");
    QTest::newRow("--compare") << QStringList { QStringLiteral("sync"), QStringLiteral("--from"),
        QStringLiteral("mem:///a"), QStringLiteral("--to"), QStringLiteral("mem:///b"),
        QStringLiteral("--compare"), QStringLiteral("content") }
                               << QStringLiteral("contents");
    QTest::newRow("--format") << QStringList { QStringLiteral("compress"), QStringLiteral("--from"),
        QStringLiteral("mem:///a"), QStringLiteral("--to"), QStringLiteral("mem:///x.tar.bz2"),
        QStringLiteral("--format"), QStringLiteral("tar.bz2") }
                              << QStringLiteral("tar.gz");
    QTest::newRow("--min-size") << QStringList { QStringLiteral("duplicates"), QStringLiteral("mem:///"),
        QStringLiteral("--min-size"), QStringLiteral("lots") }
                                << QStringLiteral("--min-size");
    QTest::newRow("--number-from") << QStringList { QStringLiteral("rename"), QStringLiteral("--in"),
        QStringLiteral("mem:///"), QStringLiteral("--number-from"), QStringLiteral("one") }
                                   << QStringLiteral("--number-from");
}

void TestMoleTasks::aValueThatIsNotOneOfTheAcceptedOnesIsRefused()
{
    QFETCH(QStringList, arguments);
    QFETCH(QString, says);

    // Three of these went through parsers written to forgive a stored file and a
    // picker, so `--mode miror --apply` ran an update sync over somebody's tree
    // and `--format tar.bz2` wrote a zip called x.tar.bz2. ADR-0028: anything
    // that can delete files does not do it on the strength of a typo.
    const Run result = run(arguments);
    QCOMPARE(result.code, BadUsage);
    QVERIFY2(result.err.contains(says), qPrintable(result.both()));
    QVERIFY2(result.out.isEmpty(), qPrintable(result.out));
}

void TestMoleTasks::aPassphraseIsNamedRatherThanTyped()
{
    QVERIFY(write(QStringLiteral("a.txt")));

    // ADR-0028 says it in as many words: secrets never appear in an argument.
    const Run typed = run({ QStringLiteral("compress"), QStringLiteral("--from"),
        uriFor(QStringLiteral("a.txt")), QStringLiteral("--to"), uriFor(QStringLiteral("out.zip")),
        QStringLiteral("--password"), QStringLiteral("hunter2") });
    QCOMPARE(typed.code, BadUsage);
    QVERIFY2(typed.err.contains(QStringLiteral("@NAME")), qPrintable(typed.both()));
    QVERIFY2(!QFile::exists(m_tree->absolute(QStringLiteral("out.zip"))), "an archive was written anyway");

    // A name whose variable is not set is refused too: taking it as "no
    // passphrase" would write an unencrypted archive where one was asked for.
    qunsetenv("MOLE_TEST_ARCHIVE_PW");
    const Run missing = run({ QStringLiteral("compress"), QStringLiteral("--from"),
        uriFor(QStringLiteral("a.txt")), QStringLiteral("--to"), uriFor(QStringLiteral("out.zip")),
        QStringLiteral("--password"), QStringLiteral("@MOLE_TEST_ARCHIVE_PW") });
    QCOMPARE(missing.code, BadUsage);
    QVERIFY2(missing.err.contains(QStringLiteral("MOLE_TEST_ARCHIVE_PW")), qPrintable(missing.both()));

    // And the parsing itself, without a drive anywhere.
    QString problem;
    qputenv("MOLE_TEST_ARCHIVE_PW", "opensesame");
    QCOMPARE(secretFromEnvironment(QStringLiteral("@MOLE_TEST_ARCHIVE_PW"), &problem),
        QStringLiteral("opensesame"));
    QVERIFY(problem.isEmpty());
    qunsetenv("MOLE_TEST_ARCHIVE_PW");
}

// ------------------------------------------ the result and everything else

void TestMoleTasks::progressGoesToStandardErrorAndTheResultToStandardOutput()
{
    QVERIFY(write(QStringLiteral("in/a.txt")));
    QVERIFY(write(QStringLiteral("in/b.txt")));
    QVERIFY(m_tree->makeDirs(QStringLiteral("out")));

    const Run result = run({ QStringLiteral("copy"), QStringLiteral("--from"),
        uriFor(QStringLiteral("in/a.txt")), QStringLiteral("--from"), uriFor(QStringLiteral("in/b.txt")),
        QStringLiteral("--to"), uriFor(QStringLiteral("out")) });

    QCOMPARE(result.code, Ok);
    // Redirecting the result has to give a file with the result in it and
    // nothing else. Nothing said which stream was which before this.
    QVERIFY2(result.out.contains(QStringLiteral("2 transferred")), qPrintable(result.both()));
    QCOMPARE(result.out.count(QLatin1Char('\n')), 1);
}

void TestMoleTasks::anInterruptedRunSaysSoAndExitsOneThirty()
{
    // 130 is the code every shell already knows, and a loop driving a transfer
    // by hand has to be able to tell it from a failure. Nothing exercised it.
    QVERIFY(write(QStringLiteral("big.bin"), QByteArray(512 * 1024, 'x')));
    QVERIFY(m_tree->makeDirs(QStringLiteral("out")));

    std::shared_ptr<FaultyFileSystem> disk = makeLocalDriveFaulty();
    QVERIFY(disk);
    disk->readStallsAt(4096);

    // Triggered by the transfer reaching the offset, never by a clock: the
    // interrupt is asked for while the stream is held, so the cancel is in place
    // before another byte moves.
    bool asked = false;
    QTimer trigger;
    trigger.setInterval(1);
    QObject::connect(&trigger, &QTimer::timeout, &trigger, [&] {
        if (asked || !disk->isStalled())
            return;
        asked = true;
        interruptMoleTasks();
        disk->release();
    });
    trigger.start();

    const Run result = run({ QStringLiteral("copy"), QStringLiteral("--from"),
        uriFor(QStringLiteral("big.bin")), QStringLiteral("--to"), uriFor(QStringLiteral("out")) });
    trigger.stop();

    QVERIFY2(asked, "the transfer never reached the offset the fault was set at");
    QCOMPARE(result.code, Interrupted);
    QVERIFY2(result.err.contains(QStringLiteral("interrupted")), qPrintable(result.both()));
}

// ------------------------------ as complete a scan as the window builds

void TestMoleTasks::aScanRecordsWhatIsInsideAContainer()
{
    const QString zipper = QStandardPaths::findExecutable(QStringLiteral("zip"));
    if (zipper.isEmpty())
        QSKIP("zip is not available to build the fixture");

    QVERIFY(m_tree->makeDirs(QStringLiteral("tree/stuff")));
    QVERIFY(write(QStringLiteral("tree/stuff/inside.txt"), QByteArray("hello")));

    QProcess packer;
    packer.setWorkingDirectory(m_tree->absolute(QStringLiteral("tree/stuff")));
    packer.start(zipper,
        { QStringLiteral("-qr"), m_tree->absolute(QStringLiteral("tree/bundle.zip")), QStringLiteral(".") });
    QVERIFY(packer.waitForFinished(30000));
    QVERIFY(QFile::exists(m_tree->absolute(QStringLiteral("tree/bundle.zip"))));
    QVERIFY(QDir(m_tree->absolute(QStringLiteral("tree/stuff"))).removeRecursively());

    // The archive backend is a plugin, so the scan has to have loaded them --
    // and the console scan built its options by hand and asked for neither.
    qputenv("MOLE_PLUGIN_PATH", QByteArray(MOLE_TEST_PLUGIN_DIR));
    const Run scanned = run({ QStringLiteral("scan"), uriFor(QStringLiteral("tree")),
        QStringLiteral("--archives"), QStringLiteral("--label"), QStringLiteral("fixture") });
    qunsetenv("MOLE_PLUGIN_PATH");

    QCOMPARE(scanned.code, Ok);
    QVERIFY2(scanned.out.contains(QStringLiteral("inside containers")), qPrintable(scanned.both()));

    // And the member is really a row, which is the whole point: a cron
    // mole-tasks scan used to build a poorer index than the window over the
    // same tree, with nothing anywhere saying so.
    QString error;
    IndexDatabase* index = m_environment->index(&error);
    QVERIFY2(index, qPrintable(error));

    // --label was read at all, which it was not: positional() is "whatever does
    // not begin with --", so the label's value counted as a second root and the
    // command was refused as "scan takes exactly one uri".
    const Result<QList<IndexVolume>> volumes = index->volumes();
    QVERIFY(volumes.ok());
    QCOMPARE(volumes.value().size(), 1);
    QCOMPARE(volumes.value().first().label, QStringLiteral("fixture"));

    SearchQuery query;
    query.add(SearchPredicate::name(QStringLiteral("inside")));
    const Result<QList<IndexSearchHit>> hits = index->search(query);
    QVERIFY(hits.ok());
    QCOMPARE(hits.value().size(), 1);
}

void TestMoleTasks::drivesCanSayWhetherThePluginsLoaded()
{
    // The first question of any report about a package -- was the network
    // backend found at all -- and it could only be answered by asking for a
    // drive and reading the failure, because plugin errors printed only when a
    // mount failed.
    qputenv("MOLE_PLUGIN_PATH", QByteArray(MOLE_TEST_PLUGIN_DIR));
    const Run result = run({ QStringLiteral("drives"), QStringLiteral("--plugins") });
    qunsetenv("MOLE_PLUGIN_PATH");

    QCOMPARE(result.code, Ok);
    QVERIFY2(result.out.contains(QStringLiteral("plugins looked for in:")), qPrintable(result.both()));
    QVERIFY2(result.out.contains(QStringLiteral(MOLE_TEST_PLUGIN_DIR)), qPrintable(result.both()));
    // Loaded or not, it says which -- and the runner does not need a plugin to
    // be there for that to be the answer.
    QVERIFY2(result.out.contains(QStringLiteral("loaded")) || result.out.contains(QStringLiteral("problems")),
        qPrintable(result.both()));
}

void TestMoleTasks::theBinaryRunsWithNoDisplay()
{
    // The claim this tool is built on is about a process: no QGuiApplication, no
    // QML, nothing that opens a connection to a display. It cannot be checked
    // from inside a test that is already running, so the binary is started with
    // the environment of a machine that has no display at all.
    const QString binary = QStringLiteral(MOLE_TASKS_BINARY);
    QVERIFY2(QFile::exists(binary), qPrintable(QStringLiteral("mole-tasks is not at %1").arg(binary)));

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("DISPLAY"));
    environment.remove(QStringLiteral("WAYLAND_DISPLAY"));
    environment.remove(QStringLiteral("QT_QPA_PLATFORM"));

    QProcess process;
    process.setProcessEnvironment(environment);
    process.start(binary, { QStringLiteral("drives") });
    QVERIFY2(process.waitForFinished(30000), "mole-tasks did not finish");

    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    QCOMPARE(process.exitCode(), int(Ok));
    QVERIFY2(output.contains(QStringLiteral("file:///")), qPrintable(output));
}

MOLE_TEST_MAIN(TestMoleTasks)
#include "tst_MoleTasks.moc"
