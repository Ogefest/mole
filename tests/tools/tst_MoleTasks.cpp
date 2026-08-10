#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "tools/tasks/Commands.h"
#include "tools/tasks/ToolEnvironment.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>

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
    void verifyAnswersForADriveThatIsThere();

    void aMountSpecCarriesItsSecretInTheEnvironment();
    void aMountSpecWithoutANameIsRefused();
    void anUnknownDriveTypeSaysTheDriveCouldNotBeReached();
    void theBinaryRunsWithNoDisplay();

private:
    Run run(const QStringList& arguments);
    QString uriFor(const QString& relative) const;
    bool write(const QString& relative, const QByteArray& contents = "payload");
    QStringList entriesIn(const QString& relative) const;

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

void TestMoleTasks::noArgumentsPrintsTheUsageAndSaysSo()
{
    const Run result = run({});
    QCOMPARE(result.code, BadUsage);
    QVERIFY2(result.out.contains(QStringLiteral("mole-tasks <command>")), qPrintable(result.both()));
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
