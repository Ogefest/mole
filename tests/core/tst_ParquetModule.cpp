#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/data/ParquetModule.h"
#include "core/data/ParquetTable.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace mole;

/// The application's side of the Parquet module: finding it, refusing one that
/// cannot be called safely, and saying why -- with nothing crashing when it is not
/// there, which is the normal state on Debian 13 and Ubuntu 24.04. See MOLE-459.
///
/// **The process's own module is made absent before anything asks for it**, by
/// pointing MOLE_PARQUET_MODULE at a file that does not exist; every case that is
/// about a particular file hands it to loadFrom() rather than relying on the
/// process-wide answer.
class TestParquetModule : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void withNoModuleParquetIsUnsupportedAndNothingCrashes();
    void theReasonForNoModuleIsSaidOnce();
    void aFileThatIsNotAModuleIsRefusedWithTheLoadersWords();
    void aModuleBuiltAgainstANewerQtIsRefusedAsBuiltForANewerSystem();
    void aModuleSpeakingAnotherInterfaceIsRefused();
    void theLoadersMessagesAreTurnedIntoReasons();
    void theRealModuleLoadsWhenThisBuildHasOne();
};

void TestParquetModule::initTestCase()
{
    qputenv("MOLE_PARQUET_MODULE", "/nonexistent/mole-parquet.so");
}

void TestParquetModule::withNoModuleParquetIsUnsupportedAndNothingCrashes()
{
    QVERIFY(!ParquetTable::isSupported());

    ParquetTable table(QStringLiteral("/nonexistent.parquet"));
    QString error;
    QVERIFY(!table.open(&error));
    QVERIFY2(error.startsWith(QStringLiteral("Parquet is not available")), qPrintable(error));
    QVERIFY(!table.isOpen());

    // Everything a grid could ask of a source that never opened, answered with
    // nothing rather than with a crash through a null reader.
    QVERIFY(table.headers().isEmpty());
    QCOMPARE(table.totalRows(), 0);
    QCOMPARE(table.matchingRows(QStringLiteral("x")), 0);
    bool readable = false;
    QVERIFY(table.rows(0, 50, {}, &readable).isEmpty());
    QVERIFY(readable);
    QVERIFY(table.columnWidths().isEmpty());
    QVERIFY(table.columnTypes().isEmpty());
    QVERIFY(table.fileSummary().isEmpty());
    QCOMPARE(table.rowsDecoded(), 0);
    table.close();
}

void TestParquetModule::theReasonForNoModuleIsSaidOnce()
{
    const QString line = ParquetTable::diagnosticLine();
    QVERIFY2(line.startsWith(QStringLiteral("parquet: not available")), qPrintable(line));
    QVERIFY2(line.contains(QStringLiteral("no Parquet module is installed")), qPrintable(line));
    QVERIFY2(line.contains(QStringLiteral("/nonexistent/mole-parquet.so")), qPrintable(line));
    // Asked again, the same answer: loaded once per process, not looked for again.
    QCOMPARE(ParquetTable::diagnosticLine(), line);
}

void TestParquetModule::aFileThatIsNotAModuleIsRefusedWithTheLoadersWords()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("mole-parquet.so"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("this is not a shared object\n");
    file.close();

    const parquetModule::State state
        = parquetModule::loadFrom({ dir.filePath(QStringLiteral("absent.so")), path });
    QVERIFY(!state.loaded);
    QVERIFY(!state.create);
    QCOMPARE(state.path, path);
    QVERIFY2(state.reason.startsWith(QStringLiteral("it could not be loaded")), qPrintable(state.reason));
}

void TestParquetModule::aModuleBuiltAgainstANewerQtIsRefusedAsBuiltForANewerSystem()
{
    // Ubuntu 26.04's module on a 24.04 that somebody gave Apache's Arrow: it may
    // well load, and calling it would run Qt 6.10's inline code against 6.4.
    const parquetModule::State state
        = parquetModule::loadFrom({ QStringLiteral(MOLE_TEST_FAKE_PARQUET_NEWER_QT) });
    QVERIFY(!state.loaded);
    QVERIFY(!state.create);
    QVERIFY2(state.reason.contains(QStringLiteral("built for a newer system than this one")),
        qPrintable(state.reason));
    QVERIFY2(state.reason.contains(QStringLiteral("it needs Qt 99.0")), qPrintable(state.reason));
}

void TestParquetModule::aModuleSpeakingAnotherInterfaceIsRefused()
{
    const parquetModule::State state
        = parquetModule::loadFrom({ QStringLiteral(MOLE_TEST_FAKE_PARQUET_OTHER_ABI) });
    QVERIFY(!state.loaded);
    QVERIFY2(state.reason.contains(QStringLiteral("built for a different Mole")), qPrintable(state.reason));
}

void TestParquetModule::theLoadersMessagesAreTurnedIntoReasons()
{
    // What glibc's loader says, by way of QLibrary, in the three cases that matter.
    // Taken from real failures rather than invented, so a reword in this file is
    // checked against what a user would actually be shown.
    const QString noArrow = parquetModule::explainLoadFailure(
        QStringLiteral("Cannot load library /usr/lib/mole/modules/mole-parquet.so: (libarrow.so.2300: cannot "
                       "open shared object "
                       "file: No such file or directory)"));
    QVERIFY2(noArrow.startsWith(QStringLiteral("the Arrow libraries it was built against are not installed")),
        qPrintable(noArrow));

    const QString olderLibstdcxx = parquetModule::explainLoadFailure(QStringLiteral(
        "Cannot load library /usr/lib/mole/modules/mole-parquet.so: (/lib/x86_64-linux-gnu/libstdc++.so.6: "
        "version `GLIBCXX_3.4.34' not found (required by /usr/lib/mole/modules/mole-parquet.so))"));
    QVERIFY2(olderLibstdcxx.startsWith(QStringLiteral("it was built for a newer system than this one")),
        qPrintable(olderLibstdcxx));

    const QString olderQt = parquetModule::explainLoadFailure(QStringLiteral(
        "Cannot load library /usr/lib/mole/modules/mole-parquet.so: (/lib/x86_64-linux-gnu/libQt6Core.so.6: "
        "version `Qt_6.10' not found (required by /usr/lib/mole/modules/mole-parquet.so))"));
    QVERIFY2(olderQt.startsWith(QStringLiteral("it was built for a newer system than this one")),
        qPrintable(olderQt));

    const QString missingSymbol = parquetModule::explainLoadFailure(QStringLiteral(
        "Cannot load library x.so: (x.so: undefined symbol: _ZN7QString6assignE14QAnyStringView)"));
    QVERIFY2(missingSymbol.startsWith(QStringLiteral("it was built for a newer system than this one")),
        qPrintable(missingSymbol));
}

void TestParquetModule::theRealModuleLoadsWhenThisBuildHasOne()
{
#ifndef MOLE_HAVE_PARQUET
    QSKIP("this build has no Arrow, so no module was built");
#else
    const parquetModule::State state = parquetModule::loadFrom({ QStringLiteral(MOLE_TEST_PARQUET_MODULE) });
    QVERIFY2(state.loaded, qPrintable(state.reason));
    QVERIFY(state.create);
    QVERIFY(state.reason.isEmpty());
    QVERIFY2(
        QRegularExpression(QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+")).match(state.arrowVersion).hasMatch(),
        qPrintable(state.arrowVersion));

    // And it makes a reader that answers for a file that is not there, without
    // throwing across the boundary.
    const QString missing = QStringLiteral("/nonexistent.parquet");
    std::unique_ptr<ParquetReader> reader(state.create(&missing));
    QVERIFY(reader);
    QString error;
    QVERIFY(!reader->open(&error));
    QVERIFY(!error.isEmpty());
#endif
}

MOLE_TEST_MAIN(TestParquetModule)
#include "tst_ParquetModule.moc"
