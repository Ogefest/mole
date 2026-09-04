#include "support/MoleTestMain.h"

#include "core/vfs/NameRules.h"

using namespace mole;

Q_DECLARE_METATYPE(mole::HostPlatform)

/// What a destination will accept in a name.
///
/// Every case here runs on every machine, because the rule set is data rather
/// than a platform: the Windows answers are the ones that have never been
/// checked anywhere, and they are the whole reason this exists.
class TestNameRules : public QObject
{
    Q_OBJECT

private slots:
    void windowsRefusesTheCharactersItRefuses_data();
    void windowsRefusesTheCharactersItRefuses();

    void windowsRefusesATrailingDotOrSpace_data();
    void windowsRefusesATrailingDotOrSpace();

    void windowsRefusesTheDeviceNames_data();
    void windowsRefusesTheDeviceNames();

    void posixAcceptsWhatWindowsWillNot_data();
    void posixAcceptsWhatWindowsWillNot();

    void anAwkwardNameIsNotAlwaysARefusedOne_data();
    void anAwkwardNameIsNotAlwaysARefusedOne();

    void someNamesAreImpossibleWhateverTheRulesSay_data();
    void someNamesAreImpossibleWhateverTheRulesSay();

    void aSuggestionIsAlwaysAcceptable_data();
    void aSuggestionIsAlwaysAcceptable();

    void theReasonNamesWhatIsWrong();
    void aLongNameIsMeasuredTheWayTheFilesystemMeasuresIt();
};

void TestNameRules::windowsRefusesTheCharactersItRefuses_data()
{
    QTest::addColumn<QString>("name");

    // Every one of them, because a list that is nearly right is a file that
    // nearly arrives.
    QTest::newRow("less than") << "a<b.txt";
    QTest::newRow("greater than") << "a>b.txt";
    QTest::newRow("colon") << "a:b.txt";
    QTest::newRow("double quote") << "say \"cheese\".jpg";
    QTest::newRow("pipe") << "a|b.txt";
    QTest::newRow("question mark") << "really?.txt";
    QTest::newRow("asterisk") << "*.txt";
    QTest::newRow("a tab") << "two\tcolumns.tsv";
    QTest::newRow("a newline") << "first\nsecond.txt";
    QTest::newRow("a bell") << "ring\aring.txt";
}

void TestNameRules::windowsRefusesTheCharactersItRefuses()
{
    QFETCH(QString, name);
    const NameVerdict verdict = checkName(name, NameRules::forPlatform(HostPlatform::Windows));
    QVERIFY2(verdict.isRejected(), qPrintable(name));
    QVERIFY(!verdict.reason.isEmpty());
    QVERIFY(!verdict.suggestion.isEmpty());
}

void TestNameRules::windowsRefusesATrailingDotOrSpace_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("rejected");

    // Windows strips these silently, so a file written as "report." arrives as
    // "report" and the caller's next read misses it.
    QTest::newRow("a trailing dot") << "report." << true;
    QTest::newRow("a trailing space") << "report " << true;
    QTest::newRow("both") << "report. " << true;
    QTest::newRow("a leading dot is fine") << ".bashrc" << false;
    QTest::newRow("a dot in the middle is fine") << "archive.tar.gz" << false;
    QTest::newRow("a leading space is fine") << " report.txt" << false;
}

void TestNameRules::windowsRefusesATrailingDotOrSpace()
{
    QFETCH(QString, name);
    QFETCH(bool, rejected);
    QCOMPARE(checkName(name, NameRules::forPlatform(HostPlatform::Windows)).isRejected(), rejected);
}

void TestNameRules::windowsRefusesTheDeviceNames_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("rejected");

    QTest::newRow("CON") << "CON" << true;
    QTest::newRow("con in lower case") << "con" << true;
    QTest::newRow("mixed case") << "CoN" << true;
    QTest::newRow("nul.txt is the device too") << "nul.txt" << true;
    QTest::newRow("COM1") << "COM1" << true;
    QTest::newRow("com9.log") << "com9.log" << true;
    QTest::newRow("LPT1") << "LPT1" << true;
    QTest::newRow("PRN") << "PRN" << true;
    QTest::newRow("AUX") << "AUX" << true;

    // Near misses that are ordinary names.
    QTest::newRow("COM10 is not reserved") << "COM10" << false;
    QTest::newRow("COM0 is not reserved") << "COM0" << false;
    QTest::newRow("console") << "console.txt" << false;
    QTest::newRow("a name containing con") << "bacon.txt" << false;
    QTest::newRow("nullify") << "nullify.txt" << false;
}

void TestNameRules::windowsRefusesTheDeviceNames()
{
    QFETCH(QString, name);
    QFETCH(bool, rejected);
    QCOMPARE(checkName(name, NameRules::forPlatform(HostPlatform::Windows)).isRejected(), rejected);
}

void TestNameRules::posixAcceptsWhatWindowsWillNot_data()
{
    QTest::addColumn<QString>("name");

    // The names the awkward-names suite is built from. Every one is legal here,
    // and every one arrives off a remote drive on a machine where it is not.
    QTest::newRow("a question mark") << "really?.txt";
    QTest::newRow("a colon") << "a:b.txt";
    QTest::newRow("double quotes") << "say \"cheese\".jpg";
    QTest::newRow("a newline") << "first\nsecond.txt";
    QTest::newRow("a trailing dot") << "report.";
    // Which "..." is, so a file really called that is a Windows problem too.
    QTest::newRow("three dots") << "...";
    QTest::newRow("a device name") << "nul.txt";
    // A separator there rather than a character in a name: copied to Windows,
    // this would not be a badly named file but a file called slash.txt inside a
    // directory called back.
    QTest::newRow("a backslash") << "back\\slash.txt";
}

void TestNameRules::posixAcceptsWhatWindowsWillNot()
{
    QFETCH(QString, name);

    for (HostPlatform platform : { HostPlatform::Posix, HostPlatform::MacOS })
        QVERIFY2(!checkName(name, NameRules::forPlatform(platform)).isRejected(), qPrintable(name));

    QVERIFY2(checkName(name, NameRules::forPlatform(HostPlatform::Windows)).isRejected(),
        qPrintable(QStringLiteral("%1 is expected to be a Windows problem").arg(name)));
}

void TestNameRules::anAwkwardNameIsNotAlwaysARefusedOne_data()
{
    QTest::addColumn<QString>("name");

    // Awkward is not the same question as refused, and conflating the two would
    // quietly shrink what the awkward-names suite covers on Windows. Every one
    // of these breaks a layer somewhere -- a quote breaks a command line, a hash
    // breaks a url -- and every one is a perfectly legal filename on all three.
    QTest::newRow("a space") << "holiday photos.jpg";
    QTest::newRow("a single quote") << "mole's notes.txt";
    QTest::newRow("a hash") << "draft #3.txt";
    QTest::newRow("an ampersand") << "this & that.txt";
    QTest::newRow("a percent") << "100% done.txt";
    QTest::newRow("a leading dash") << "-rf.txt";
    QTest::newRow("something that looks encoded") << "already%20encoded%2Fname.txt";
    QTest::newRow("emoji") << QString::fromUtf8("holiday \xF0\x9F\x8F\x96\xEF\xB8\x8F.jpg");
    QTest::newRow("combining characters") << QString::fromUtf8("cafe\xCC\x81.txt");
    QTest::newRow("a 255-character name") << QString(251, QLatin1Char('n')) + QStringLiteral(".txt");
}

void TestNameRules::anAwkwardNameIsNotAlwaysARefusedOne()
{
    QFETCH(QString, name);
    for (HostPlatform platform : { HostPlatform::Posix, HostPlatform::MacOS, HostPlatform::Windows }) {
        const NameVerdict verdict = checkName(name, NameRules::forPlatform(platform));
        QVERIFY2(!verdict.isRejected(), qPrintable(QStringLiteral("%1: %2").arg(name, verdict.reason)));
    }
}

void TestNameRules::someNamesAreImpossibleWhateverTheRulesSay_data()
{
    QTest::addColumn<QString>("name");

    QTest::newRow("empty") << "";
    QTest::newRow("a dot") << ".";
    QTest::newRow("two dots") << "..";
    QTest::newRow("a separator") << "a/b.txt";
    QTest::newRow("a null") << QString::fromUtf16(u"a\0b.txt", 7);
}

void TestNameRules::someNamesAreImpossibleWhateverTheRulesSay()
{
    QFETCH(QString, name);

    // Refused before the rules are consulted, including by the permissive set a
    // backend gets when it says nothing.
    for (HostPlatform platform : { HostPlatform::Posix, HostPlatform::MacOS, HostPlatform::Windows }) {
        QVERIFY2(checkName(name, NameRules::forPlatform(platform)).isRejected(), qPrintable(name));
    }
    QVERIFY(checkName(name, NameRules()).isRejected());
}

void TestNameRules::aSuggestionIsAlwaysAcceptable_data()
{
    QTest::addColumn<QString>("name");

    QTest::newRow("one forbidden character") << "a:b.txt";
    QTest::newRow("several") << "a:b*c?.txt";
    QTest::newRow("a control character") << "two\tcolumns.tsv";
    QTest::newRow("a trailing dot") << "report.";
    QTest::newRow("a device name") << "nul.txt";
    QTest::newRow("a device name with no extension") << "CON";
    QTest::newRow("everything at once") << "co:n?. ";
    QTest::newRow("too long") << QString(400, QLatin1Char('n'));
}

void TestNameRules::aSuggestionIsAlwaysAcceptable()
{
    QFETCH(QString, name);

    const NameRules windows = NameRules::forPlatform(HostPlatform::Windows);
    const NameVerdict verdict = checkName(name, windows);
    QVERIFY2(verdict.isRejected(), qPrintable(name));
    QVERIFY2(!verdict.suggestion.isEmpty(), qPrintable(name));

    // The point of offering one. A suggestion the rules would refuse in turn is
    // worse than none, because it looks like an answer.
    const NameVerdict second = checkName(verdict.suggestion, windows);
    QVERIFY2(!second.isRejected(),
        qPrintable(QStringLiteral("\"%1\" was suggested for \"%2\" and is refused: %3")
                       .arg(verdict.suggestion, name, second.reason)));
}

void TestNameRules::theReasonNamesWhatIsWrong()
{
    // "this name is invalid" tells somebody staring at a hundred rows nothing
    // they can act on, so the character is in the sentence.
    const NameRules windows = NameRules::forPlatform(HostPlatform::Windows);

    QVERIFY(checkName(QStringLiteral("a:b.txt"), windows).reason.contains(QLatin1Char(':')));
    QVERIFY(checkName(QStringLiteral("really?.txt"), windows).reason.contains(QLatin1Char('?')));
    QVERIFY(checkName(QStringLiteral("report."), windows).reason.contains(QStringLiteral("dot")));
    QVERIFY(checkName(QStringLiteral("report "), windows).reason.contains(QStringLiteral("space")));
    QVERIFY(checkName(QStringLiteral("nul.txt"), windows).reason.contains(QStringLiteral("nul")));
    QVERIFY(
        checkName(QStringLiteral("two\tcolumns.tsv"), windows).reason.contains(QStringLiteral("control")));
}

/// A name the rules accepted and the kernel refused.
///
/// `forPlatform(Posix).maximumLength` was 255 counted in *characters*, and ext4,
/// XFS, btrfs and APFS all count bytes -- so a 200-character Polish name is
/// 300-odd bytes, passed checkName(), and was refused by the kernel on the way
/// to disk. That is the outcome ADR-0070 set out to remove: the preview promises
/// what the filesystem will accept. PartialWrite.cpp two files away was already
/// measuring toUtf8().size() against the same 255, so the two halves of the same
/// application disagreed about what fits. See MOLE-359.
void TestNameRules::aLongNameIsMeasuredTheWayTheFilesystemMeasuresIt()
{
    const NameRules posix = NameRules::forPlatform(HostPlatform::Posix);

    // Two bytes each in UTF-8, so 200 of them is 400 bytes: comfortably inside a
    // 255-character limit and comfortably outside a 255-byte one.
    const QString polish(200, QChar(u'ł'));
    QCOMPARE(polish.size(), 200);
    QCOMPARE(polish.toUtf8().size(), 400);

    const NameVerdict verdict = checkName(polish, posix);
    QVERIFY2(verdict.isRejected(), "a name the filesystem will refuse was accepted");
    QVERIFY2(verdict.reason.contains(QStringLiteral("bytes")), qPrintable(verdict.reason));
    // The offered name fits, and is not cut through the middle of a character.
    QVERIFY(!verdict.suggestion.isEmpty());
    QVERIFY(verdict.suggestion.toUtf8().size() <= 255);
    QCOMPARE(verdict.suggestion, QString(127, QChar(u'ł')));
    QVERIFY(!checkName(verdict.suggestion, posix).isRejected());

    // 255 ASCII characters is 255 bytes and still fits, which is what says this
    // is a change of unit and not of limit.
    QVERIFY(!checkName(QString(255, QLatin1Char('a')), posix).isRejected());
    QVERIFY(checkName(QString(256, QLatin1Char('a')), posix).isRejected());

    // Windows counts UTF-16 code units, and 200 of these is 200 of those.
    const NameRules windows = NameRules::forPlatform(HostPlatform::Windows);
    QVERIFY2(!checkName(polish, windows).isRejected(),
        "NTFS counts characters, so a 200-character name fits there");
}

MOLE_TEST_MAIN(TestNameRules)
#include "tst_NameRules.moc"
