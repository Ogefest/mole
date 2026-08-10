#include "support/MoleTestMain.h"

#include "core/data/FileType.h"

#include <QMimeDatabase>

using namespace mole;

namespace {

/// Every sample is built here rather than committed as a fixture: a binary blob
/// in the tree is one nobody can review, and the bytes that matter about a JPEG
/// are the first four of them.
QByteArray jpeg()
{
    return QByteArray::fromHex("ffd8ffe000104a46494600010100") + QByteArray(200, ' ');
}

QByteArray png()
{
    return QByteArray::fromHex("89504e470d0a1a0a0000000d49484452") + QByteArray(200, '\x08');
}

QByteArray zip()
{
    return QByteArrayLiteral("PK\x03\x04") + QByteArray(200, '\x01');
}

QByteArray sqlite()
{
    return QByteArray("SQLite format 3\0", 16) + QByteArray(200, '\x02');
}

/// A 64-bit relocatable object: the class, the endianness and `e_type` = ET_REL
/// at offset 16 are what tell it apart from an executable.
QByteArray elfObject()
{
    QByteArray elf(128, '\0');
    elf.replace(0, 4,
        QByteArrayLiteral("\x7f"
                          "ELF"));
    elf[4] = 2;
    elf[5] = 1;
    elf[6] = 1;
    elf[16] = 1;
    elf[18] = 0x3e;
    return elf;
}

bool isText(const QString& mimeType)
{
    static const QMimeDatabase mimeDatabase;
    return mimeDatabase.mimeTypeForName(mimeType).inherits(QStringLiteral("text/plain"));
}

} // namespace

/// What a file is, decided from a page of it rather than from its name.
class TestFileType : public QObject
{
    Q_OBJECT

private slots:
    void textWhateverItIsCalled_data();
    void textWhateverItIsCalled();
    void aByteOrderMarkIsText();
    void binaryFormatsAreThemselves_data();
    void binaryFormatsAreThemselves();
    void contentBeatsAMisleadingName();
    void aMoreSpecificNameSurvivesItsMagic();
    void nulBytesAreBinaryAndNothingIsText();
    void aSampleCutMidCharacterIsStillText();
    void aStrayControlCharacterDoesNotMakeALogBinary();
};

// ------------------------------------------------------------------- text

void TestFileType::textWhateverItIsCalled_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("head");

    // The names shared-mime-info 2.4 has no glob for. Each one is text, and each
    // one used to read "Unknown".
    QTest::newRow("Dockerfile") << "Dockerfile" << QByteArray("FROM debian:bookworm\nRUN apt-get update\n");
    QTest::newRow("gitignore") << ".gitignore" << QByteArray("build/\n*.o\n.cache/\n");
    QTest::newRow("LICENSE") << "LICENSE"
                             << QByteArray("MIT License\n\nPermission is hereby granted, free of charge\n");
    QTest::newRow("bashrc") << ".bashrc" << QByteArray("export PS1='$ '\nalias ll='ls -l'\n");
    QTest::newRow("Jenkinsfile") << "Jenkinsfile" << QByteArray("pipeline { agent any }\n");
    QTest::newRow("editorconfig") << ".editorconfig"
                                  << QByteArray("root = true\n\n[*.cpp]\nindent_size = 4\n");
    QTest::newRow("no name at all") << "blob8842" << QByteArray("plain enough prose, and nothing else\n");

    // Not UTF-8 and not required to be: a log written by something that never
    // heard of Unicode is still a log.
    QTest::newRow("Latin-1") << "logfile" << QByteArray("caf\xe9 na\xefve\nstarted at 08:00\n");

    // A name the database does know keeps its own, more specific answer.
    QTest::newRow("Makefile") << "Makefile" << QByteArray("all:\n\tgcc -o x x.c\n");
    QTest::newRow("CMakeLists.txt") << "CMakeLists.txt" << QByteArray("project(mole)\n");
    QTest::newRow("json") << "config.json" << QByteArray("{\"a\": 1}");
}

void TestFileType::textWhateverItIsCalled()
{
    QFETCH(QString, name);
    QFETCH(QByteArray, head);

    const QString type = FileType::identify(name, head);
    // "Is it text" is the question the preview layer asks, and the exact name is
    // the database's business -- a later shared-mime-info may well add a glob for
    // Dockerfile and answer text/x-dockerfile.
    QVERIFY2(isText(type), qPrintable(QStringLiteral("%1 -> %2").arg(name, type)));
}

void TestFileType::aByteOrderMarkIsText()
{
    const QByteArray utf16 = QByteArrayLiteral("\xff\xfe") + QByteArray("h\0e\0l\0l\0o\0", 10);
    QVERIFY(FileType::looksLikeText(utf16));
    QVERIFY(isText(FileType::identify(QStringLiteral("notes"), utf16)));

    QVERIFY(FileType::looksLikeText(QByteArrayLiteral("\xef\xbb\xbf") + QByteArray("hello")));

    // Without the mark the same bytes are half NUL, and nothing here promises to
    // recognise them -- only that a decision is made and it is not a crash.
    const QByteArray headless = QByteArray("h\0e\0l\0l\0o\0", 10);
    QVERIFY(!FileType::identify(QStringLiteral("notes"), headless).isEmpty());
}

// ----------------------------------------------------------------- binary

void TestFileType::binaryFormatsAreThemselves_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("head");
    QTest::addColumn<QString>("mimeType");

    QTest::newRow("jpeg") << "photo.jpg" << jpeg() << "image/jpeg";
    QTest::newRow("png") << "photo.png" << png() << "image/png";
    QTest::newRow("zip") << "archive.zip" << zip() << "application/zip";
    QTest::newRow("sqlite") << "index.sqlite" << sqlite() << "application/vnd.sqlite3";
    QTest::newRow("elf object") << "walker.o" << elfObject() << "application/x-object";

    // And the same bytes under a name that says nothing: this is the half that
    // could not be answered at all before.
    QTest::newRow("jpeg, unnamed") << "blob1" << jpeg() << "image/jpeg";
    QTest::newRow("sqlite, unnamed") << "blob2" << sqlite() << "application/vnd.sqlite3";
    QTest::newRow("elf object, unnamed") << "blob3" << elfObject() << "application/x-object";
}

void TestFileType::binaryFormatsAreThemselves()
{
    QFETCH(QString, name);
    QFETCH(QByteArray, head);
    QFETCH(QString, mimeType);

    QCOMPARE(FileType::identify(name, head), mimeType);
    QVERIFY(!isText(mimeType));
}

void TestFileType::contentBeatsAMisleadingName()
{
    // The case the second pass exists for. A name is a label somebody typed.
    QCOMPARE(FileType::identify(QStringLiteral("notes.txt"), jpeg()), QStringLiteral("image/jpeg"));
    QCOMPARE(FileType::identify(QStringLiteral("notes.txt"), zip()), QStringLiteral("application/zip"));

    // And the other way round: a text file called photo.jpg is text, so it opens
    // in something that can show it rather than in an empty frame.
    QCOMPARE(FileType::identify(QStringLiteral("photo.jpg"), QByteArray("not a photograph at all\n")),
        QStringLiteral("text/plain"));
}

void TestFileType::aMoreSpecificNameSurvivesItsMagic()
{
    // A .docx is a zip, and the magic rules can only see the zip. The name knows
    // more, and its answer is a subclass of what the bytes said rather than a
    // contradiction of it, so it stands.
    const QByteArray docx = QByteArrayLiteral("PK\x03\x04") + QByteArray(30, '\0')
        + QByteArrayLiteral("[Content_Types].xml") + QByteArray(100, '\x01');
    const QString type = FileType::identify(QStringLiteral("report.docx"), docx);
    QCOMPARE(type, QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document"));

    static const QMimeDatabase mimeDatabase;
    QVERIFY(mimeDatabase.mimeTypeForName(type).inherits(QStringLiteral("application/zip")));
}

// ------------------------------------------------------------------ edges

void TestFileType::nulBytesAreBinaryAndNothingIsText()
{
    const QByteArray nuls(FileType::kSampleBytes, '\0');
    QVERIFY(!FileType::looksLikeText(nuls));
    QVERIFY(!isText(FileType::identify(QStringLiteral("blob"), nuls)));

    QVERIFY(FileType::looksLikeText(QByteArray()));
    QCOMPARE(FileType::identify(QStringLiteral("blob"), QByteArray()), QStringLiteral("text/plain"));

    // One NUL is enough, wherever it is. A file that is text for a page and then
    // is not is not a text file.
    QByteArray mostlyText = QByteArray("plain prose for a good long while, and then ") + QByteArray(1, '\0');
    QVERIFY(!FileType::looksLikeText(mostlyText));
}

void TestFileType::aSampleCutMidCharacterIsStillText()
{
    // What a 4 kB read of a UTF-8 file does about one time in four.
    const QByteArray full = QString::fromUtf8("héllo wörld — a dash and an ümlaut\n").repeated(20).toUtf8();
    for (int cut = 1; cut <= 3; ++cut) {
        const QByteArray sample = full.left(full.size() - cut);
        QVERIFY2(FileType::looksLikeText(sample), qPrintable(QStringLiteral("cut %1").arg(cut)));
        QVERIFY(isText(FileType::identify(QStringLiteral("prose"), sample)));
    }
}

void TestFileType::aStrayControlCharacterDoesNotMakeALogBinary()
{
    // A colouring escape or a bell in a log. The threshold is what this is about,
    // so it is stated rather than implied.
    QByteArray log;
    while (log.size() < 2000)
        log += QByteArray("\x1b[32mINFO\x1b[0m  started a thing that took a while, and said so at "
                          "some length, in the way that a log written by a real program does\n");
    const double share = 100.0 * log.count('\x1b') / log.size();
    QVERIFY2(share < FileType::kControlPercent, qPrintable(QStringLiteral("%1%").arg(share)));
    QVERIFY(FileType::looksLikeText(log));

    // Past the threshold it is binary, which is the point of having one.
    QByteArray dense;
    while (dense.size() < 2000)
        dense += QByteArray("abcdefghij\x01\x02\x03\x04\x05");
    QVERIFY(!FileType::looksLikeText(dense));
}

MOLE_TEST_MAIN(TestFileType)
#include "tst_FileType.moc"
