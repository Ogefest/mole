#include "support/FileSystemConformance.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/vfs/backends/LocalFileSystem.h"

#include <QFile>

using namespace mole;
using namespace mole::test;

/// The conformance suite again, with the large-payload rows switched on.
///
/// **The same suite, not a second one.** What is being asked is whether the
/// contract still holds at a size, and a separate set of assertions would be a
/// second contract that drifts from the first. `exercisesScale` turns on the
/// three rows the fast tier cannot afford: a file past the 4 GiB line where a
/// 32-bit offset stops working, a directory of a hundred thousand entries where
/// anything quadratic stops finishing, and a name at exactly the length the
/// volume states.
///
/// Heavy-tier, so `make test` does not pay for it -- it writes four gigabytes
/// and a hundred thousand files, and it skips the parts the scratch volume
/// cannot hold rather than failing. See MOLE-401.
class TestScaleConformance : public QObject
{
    Q_OBJECT

private slots:
    void conformanceAtScale();
};

void TestScaleConformance::conformanceAtScale()
{
    TempTree tree;
    QVERIFY(tree.isValid());

    ConformanceContext context;
    context.fileSystem = std::make_shared<LocalFileSystem>();
    context.root = tree.rootUri();
    context.seedFile
        = [&tree](const QString& path, const QByteArray& data) { return tree.writeFile(path, data); };
    context.seedDir = [&tree](const QString& path) { return tree.makeDirs(path); };
    context.whileUnlistable = [&tree](const QString& path, const std::function<void()>& check) {
        const QString absolute = tree.absolute(path);
        if (!madeUnreadable(absolute))
            return false;
        check();
        QFile::setPermissions(
            absolute, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        return true;
    };
    context.exercisesScale = true;

    runFileSystemConformance(context);
}

MOLE_TEST_MAIN(TestScaleConformance)
#include "tst_ScaleConformance.moc"
