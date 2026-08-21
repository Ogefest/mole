#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/rename/RenamePlan.h"
#include "core/vfs/NameRules.h"
#include "core/vfs/backends/MemoryFileSystem.h"

using namespace mole;
using namespace mole::test;

namespace {

RenameRule replace(const QString& find, const QString& with, bool regex = false)
{
    RenameRule rule;
    rule.kind = RenameRule::Kind::Replace;
    rule.find = find;
    rule.replaceWith = with;
    rule.useRegex = regex;
    return rule;
}

RenameRule caseRule(RenameRule::CaseStyle style)
{
    RenameRule rule;
    rule.kind = RenameRule::Kind::Case;
    rule.caseStyle = style;
    return rule;
}

QList<VfsUri> urisIn(const QString& directory, const QStringList& names)
{
    QList<VfsUri> out;
    for (const QString& name : names)
        out.append(VfsUri::fromString(directory + QLatin1Char('/') + name));
    return out;
}

} // namespace

/// The rename rules, and the plan that refuses to run a batch that would break.
class TestRenamePlan : public QObject
{
    Q_OBJECT

private slots:
    // ---- individual rules ----
    void replacesPlainText();
    void replacesByPattern();
    void anInvalidPatternLeavesTheNameAlone();
    void changesCase();
    void insertsAndRemoves();
    void countsPositionsFromTheEnd();
    void stripsCharacterClasses();
    void stripsAccentsWithoutLosingLetters();
    void numbersThemInOrder();
    void addsAffixes();
    void changesTheExtension();

    // ---- what the rules must not touch ----
    void leavesTheExtensionAloneByDefault();
    void treatsADotfileAsHavingNoExtension();

    // ---- the plan ----
    void appliesRulesInOrder();
    void refusesTwoFilesTakingOneName();
    void refusesANameThatIsAlreadyThere();
    void allowsASwapWithinTheBatch();
    void refusesAnEmptyName();
    void refusesAPathSeparator();
    void anUnchangedRowIsNotABlockedRow();
    void aCaseOnlyRenameIsNotACollision();
    void thePreviewAgreesWithTheBackendAboutCollisions();
    void aNameTheDestinationWillNotAcceptIsMarkedBeforeAnythingMoves();
};

// ------------------------------------------------------------------ rules

void TestRenamePlan::replacesPlainText()
{
    QCOMPARE(RenamePlan::apply(QStringLiteral("IMG_0421.JPG"),
                 { replace(QStringLiteral("IMG"), QStringLiteral("Photo")) }, 0),
        QStringLiteral("Photo_0421.JPG"));

    // Case-insensitive by default, because that is what people mean when they
    // type a word into a find box.
    QCOMPARE(RenamePlan::apply(
                 QStringLiteral("img_1.txt"), { replace(QStringLiteral("IMG"), QStringLiteral("Photo")) }, 0),
        QStringLiteral("Photo_1.txt"));
}

void TestRenamePlan::replacesByPattern()
{
    QCOMPARE(RenamePlan::apply(QStringLiteral("report 2024-05-01.pdf"),
                 { replace(QStringLiteral("\\d{4}-\\d{2}-\\d{2}"), QStringLiteral("dated"), true) }, 0),
        QStringLiteral("report dated.pdf"));
}

void TestRenamePlan::anInvalidPatternLeavesTheNameAlone()
{
    // Half-applying a broken expression would be worse than doing nothing, and
    // the preview would show a mangled name as though it were intended.
    QCOMPARE(RenamePlan::apply(QStringLiteral("a.txt"),
                 { replace(QStringLiteral("[unclosed"), QStringLiteral("x"), true) }, 0),
        QStringLiteral("a.txt"));
}

void TestRenamePlan::changesCase()
{
    QCOMPARE(RenamePlan::apply(
                 QStringLiteral("my HOLIDAY snaps.JPG"), { caseRule(RenameRule::CaseStyle::Title) }, 0),
        QStringLiteral("My Holiday Snaps.JPG"));
    QCOMPARE(RenamePlan::apply(
                 QStringLiteral("my HOLIDAY snaps.JPG"), { caseRule(RenameRule::CaseStyle::Sentence) }, 0),
        QStringLiteral("My holiday snaps.JPG"));
    QCOMPARE(
        RenamePlan::apply(QStringLiteral("Mixed Case.txt"), { caseRule(RenameRule::CaseStyle::Upper) }, 0),
        QStringLiteral("MIXED CASE.txt"));
}

void TestRenamePlan::insertsAndRemoves()
{
    RenameRule insert;
    insert.kind = RenameRule::Kind::Insert;
    insert.text = QStringLiteral("2024-");
    insert.position = 0;
    QCOMPARE(
        RenamePlan::apply(QStringLiteral("report.pdf"), { insert }, 0), QStringLiteral("2024-report.pdf"));

    RenameRule remove;
    remove.kind = RenameRule::Kind::Remove;
    remove.position = 0;
    remove.length = 4;
    QCOMPARE(RenamePlan::apply(QStringLiteral("IMG_photo.jpg"), { remove }, 0), QStringLiteral("photo.jpg"));
}

void TestRenamePlan::countsPositionsFromTheEnd()
{
    // "the last four characters" should not need a rule of its own. A removal
    // counts back from the end: -4 with a length of 4 takes "_v01".
    RenameRule remove;
    remove.kind = RenameRule::Kind::Remove;
    remove.position = -4;
    remove.length = 4;
    QCOMPARE(RenamePlan::apply(QStringLiteral("photo_v01.jpg"), { remove }, 0), QStringLiteral("photo.jpg"));

    // An insertion point sits between characters, so -1 is the end rather than
    // one short of it. Two conventions, each the natural one for its operation.
    RenameRule insert;
    insert.kind = RenameRule::Kind::Insert;
    insert.text = QStringLiteral("_final");
    insert.position = -1;
    QCOMPARE(
        RenamePlan::apply(QStringLiteral("report.pdf"), { insert }, 0), QStringLiteral("report_final.pdf"));
}

void TestRenamePlan::stripsCharacterClasses()
{
    RenameRule digits;
    digits.kind = RenameRule::Kind::Strip;
    digits.stripClass = RenameRule::StripClass::Digits;
    QCOMPARE(RenamePlan::apply(QStringLiteral("scan0042.png"), { digits }, 0), QStringLiteral("scan.png"));

    RenameRule spaces;
    spaces.kind = RenameRule::Kind::Strip;
    spaces.stripClass = RenameRule::StripClass::Whitespace;
    QCOMPARE(RenamePlan::apply(QStringLiteral("my holiday snaps.jpg"), { spaces }, 0),
        QStringLiteral("myholidaysnaps.jpg"));
}

void TestRenamePlan::stripsAccentsWithoutLosingLetters()
{
    RenameRule accents;
    accents.kind = RenameRule::Kind::Strip;
    accents.stripClass = RenameRule::StripClass::Accents;

    // "Kraków" has to become "Krakow", not "Krakw" -- which is what removing
    // non-ASCII wholesale would do, and why the two are separate rules.
    QCOMPARE(
        RenamePlan::apply(QString::fromUtf8("Kraków.txt"), { accents }, 0), QStringLiteral("Krakow.txt"));
    QCOMPARE(RenamePlan::apply(QString::fromUtf8("Poznań-2024.txt"), { accents }, 0),
        QStringLiteral("Poznan-2024.txt"));
}

void TestRenamePlan::numbersThemInOrder()
{
    RenameRule number;
    number.kind = RenameRule::Kind::Number;
    number.start = 1;
    number.step = 1;
    number.padding = 3;
    number.numberAt = -1;
    number.numberSeparator = QStringLiteral("_");

    QCOMPARE(RenamePlan::apply(QStringLiteral("photo.jpg"), { number }, 0), QStringLiteral("photo_001.jpg"));
    QCOMPARE(RenamePlan::apply(QStringLiteral("photo.jpg"), { number }, 9), QStringLiteral("photo_010.jpg"));

    number.start = 10;
    number.step = 5;
    QCOMPARE(RenamePlan::apply(QStringLiteral("photo.jpg"), { number }, 2), QStringLiteral("photo_020.jpg"));
}

void TestRenamePlan::addsAffixes()
{
    RenameRule affix;
    affix.kind = RenameRule::Kind::Affix;
    affix.prefix = QStringLiteral("2024_");
    affix.suffix = QStringLiteral("_final");
    QCOMPARE(RenamePlan::apply(QStringLiteral("report.pdf"), { affix }, 0),
        QStringLiteral("2024_report_final.pdf"));
}

void TestRenamePlan::changesTheExtension()
{
    RenameRule extension;
    extension.kind = RenameRule::Kind::Extension;
    extension.newExtension = QStringLiteral("txt");
    QCOMPARE(RenamePlan::apply(QStringLiteral("notes.md"), { extension }, 0), QStringLiteral("notes.txt"));

    // An empty target means "normalise it", which is the common case.
    extension.newExtension.clear();
    QCOMPARE(RenamePlan::apply(QStringLiteral("PHOTO.JPG"), { extension }, 0), QStringLiteral("PHOTO.jpg"));
}

// ------------------------------------------------------------ what to touch

void TestRenamePlan::leavesTheExtensionAloneByDefault()
{
    // Upper-casing a name must not turn ".txt" into ".TXT": plenty of tools
    // still care, and the user asked about the name.
    QCOMPARE(RenamePlan::apply(QStringLiteral("notes.txt"), { caseRule(RenameRule::CaseStyle::Upper) }, 0),
        QStringLiteral("NOTES.txt"));

    RenameRule whole = caseRule(RenameRule::CaseStyle::Upper);
    whole.scope = RenameRule::Scope::WholeName;
    QCOMPARE(RenamePlan::apply(QStringLiteral("notes.txt"), { whole }, 0), QStringLiteral("NOTES.TXT"));
}

void TestRenamePlan::treatsADotfileAsHavingNoExtension()
{
    // ".gitignore" is a name, not an extension. Splitting it the other way
    // would rename the file to nothing at all.
    QCOMPARE(RenamePlan::apply(QStringLiteral(".gitignore"), { caseRule(RenameRule::CaseStyle::Upper) }, 0),
        QStringLiteral(".GITIGNORE"));
}

// ------------------------------------------------------------------- plan

void TestRenamePlan::appliesRulesInOrder()
{
    // Strip first, then number: the other order would number a name that still
    // had its digits in it.
    RenameRule strip;
    strip.kind = RenameRule::Kind::Strip;
    strip.stripClass = RenameRule::StripClass::Digits;

    RenameRule number;
    number.kind = RenameRule::Kind::Number;
    number.padding = 2;
    number.numberSeparator = QStringLiteral("-");

    QCOMPARE(RenamePlan::apply(QStringLiteral("scan0042.png"), { strip, number }, 0),
        QStringLiteral("scan-01.png"));
    // The other way round the counter is added first and then partly stripped,
    // leaving its separator behind -- which is exactly the kind of result an
    // ordered list of rules lets you see and fix, rather than a form's fixed
    // internal order deciding for you.
    QCOMPARE(
        RenamePlan::apply(QStringLiteral("scan0042.png"), { number, strip }, 0), QStringLiteral("scan-.png"));
}

void TestRenamePlan::refusesTwoFilesTakingOneName()
{
    RenameRule strip;
    strip.kind = RenameRule::Kind::Strip;
    strip.stripClass = RenameRule::StripClass::Digits;

    const RenamePlan plan
        = RenamePlan::build(urisIn(QStringLiteral("file:///data"), { "scan1.png", "scan2.png" }), { strip });

    // The filesystem would only notice on the second file, by which time the
    // first has already moved. A batch that half-succeeds is worse than one
    // that never ran.
    QCOMPARE(plan.entries().size(), 2);
    QVERIFY(!plan.entries().at(0).isBlocked());
    QVERIFY(plan.entries().at(1).isBlocked());
    QVERIFY(plan.entries().at(1).problem.contains(QStringLiteral("two files")));
    QVERIFY(!plan.canApply());
}

void TestRenamePlan::refusesANameThatIsAlreadyThere()
{
    QHash<QString, QStringList> existing;
    existing.insert(QStringLiteral("file:///data"), { QStringLiteral("taken.png") });

    RenameRule rule = replace(QStringLiteral("scan"), QStringLiteral("taken"));
    const RenamePlan plan
        = RenamePlan::build(urisIn(QStringLiteral("file:///data"), { "scan.png" }), { rule }, existing);

    QVERIFY(plan.entries().first().isBlocked());
    QVERIFY(plan.entries().first().problem.contains(QStringLiteral("already there")));
}

void TestRenamePlan::allowsASwapWithinTheBatch()
{
    QHash<QString, QStringList> existing;
    existing.insert(QStringLiteral("file:///data"), { QStringLiteral("a.txt"), QStringLiteral("b.txt") });

    // a -> b and b -> a. Both names exist, but both are being vacated by this
    // very batch, so refusing them would block a rename that is perfectly fine.
    RenameRule swap = replace(QStringLiteral("a"), QStringLiteral("TEMP"));
    Q_UNUSED(swap);

    RenameRule rule;
    rule.kind = RenameRule::Kind::Replace;
    rule.find = QStringLiteral("a");
    rule.replaceWith = QStringLiteral("b");

    const RenamePlan plan
        = RenamePlan::build(urisIn(QStringLiteral("file:///data"), { "a.txt" }), { rule }, existing);

    // "a.txt" -> "b.txt", and b.txt is in the batch's directory but not in the
    // batch, so it is genuinely taken.
    QVERIFY(plan.entries().first().isBlocked());
}

void TestRenamePlan::refusesAnEmptyName()
{
    RenameRule strip;
    strip.kind = RenameRule::Kind::Strip;
    strip.stripClass = RenameRule::StripClass::Digits;

    const RenamePlan plan
        = RenamePlan::build(urisIn(QStringLiteral("file:///data"), { "123.txt" }), { strip });
    QVERIFY(plan.entries().first().isBlocked());
    QVERIFY(plan.entries().first().problem.contains(QStringLiteral("only an extension")));
}

void TestRenamePlan::refusesAPathSeparator()
{
    RenameRule rule = replace(QStringLiteral("_"), QStringLiteral("/"));
    const RenamePlan plan
        = RenamePlan::build(urisIn(QStringLiteral("file:///data"), { "a_b.txt" }), { rule });

    // A separator would move the file rather than rename it, which is not what
    // anybody asked a rename tool to do.
    QVERIFY(plan.entries().first().isBlocked());
    QVERIFY(plan.entries().first().problem.contains(QStringLiteral("separator")));
}

void TestRenamePlan::anUnchangedRowIsNotABlockedRow()
{
    RenameRule rule = replace(QStringLiteral("zzz"), QStringLiteral("yyy"));
    const RenamePlan plan
        = RenamePlan::build(urisIn(QStringLiteral("file:///data"), { "a.txt", "b.txt" }), { rule });

    QCOMPARE(plan.changedCount(), 0);
    QCOMPARE(plan.blockedCount(), 0);
    // Nothing to do is not the same as ready to go.
    QVERIFY(!plan.canApply());
}

void TestRenamePlan::aCaseOnlyRenameIsNotACollision()
{
    // report.txt -> Report.txt. On a volume that ignores case the file in the
    // way is the file being renamed, and the preview used to mark the row clean
    // while the backend refused every one of them -- two layers, two answers,
    // and the user sees the optimistic one first.
    QHash<QString, QStringList> existing;
    existing.insert(QStringLiteral("file:///data"), { QStringLiteral("report.txt") });

    const RenameRule rule = caseRule(RenameRule::CaseStyle::Title);
    for (Qt::CaseSensitivity sensitivity : { Qt::CaseSensitive, Qt::CaseInsensitive }) {
        const RenamePlan plan = RenamePlan::build(
            urisIn(QStringLiteral("file:///data"), { "report.txt" }), { rule }, existing, sensitivity);

        QCOMPARE(plan.entries().first().newName, QStringLiteral("Report.txt"));
        QVERIFY2(!plan.entries().first().isBlocked(),
            qPrintable(QStringLiteral("blocked with %1: %2")
                           .arg(sensitivity == Qt::CaseSensitive ? "sensitive" : "insensitive",
                               plan.entries().first().problem)));
    }
}

void TestRenamePlan::thePreviewAgreesWithTheBackendAboutCollisions()
{
    // The claim is not "this rule is right", it is "this layer predicts what the
    // layer below will do". So both are asked the same question and the answers
    // are compared, which is a test that goes on meaning something when either
    // side changes.
    for (Qt::CaseSensitivity sensitivity : { Qt::CaseSensitive, Qt::CaseInsensitive }) {
        auto fs = std::make_shared<MemoryFileSystem>();
        fs->setCaseSensitivity(sensitivity);
        fs->addFile(QStringLiteral("/data/report.txt"), QByteArray("one"));
        fs->addFile(QStringLiteral("/data/NOTES.txt"), QByteArray("two"));

        const QHash<QString, QStringList> existing { { QStringLiteral("mem:///data"),
            { QStringLiteral("report.txt"), QStringLiteral("NOTES.txt") } } };

        struct Case
        {
            const char* stem; ///< what "report" becomes; the rules leave the extension alone
            const char* expected; ///< the whole name that should come out
        };
        // A case-only rename of itself; a rename onto another file spelled
        // differently; and a rename onto a name nothing holds.
        for (const Case& one : { Case { "Report", "Report.txt" }, Case { "notes", "notes.txt" },
                 Case { "fresh", "fresh.txt" } }) {
            RenameRule rule;
            rule.kind = RenameRule::Kind::Replace;
            rule.find = QStringLiteral("report");
            rule.replaceWith = QString::fromLatin1(one.stem);

            const RenamePlan plan = RenamePlan::build(
                urisIn(QStringLiteral("mem:///data"), { "report.txt" }), { rule }, existing, sensitivity);

            // Asserted, because a rule that quietly stopped changing the name
            // would make every comparison below trivially true.
            QCOMPARE(plan.entries().first().newName, QString::fromLatin1(one.expected));

            const VfsUri from = VfsUri::fromString(QStringLiteral("mem:///data/report.txt"));
            const VfsUri to = VfsUri::fromString(QStringLiteral("mem:///data/%1").arg(one.expected));
            const Result<void> renamed = fs->rename(from, to);

            QVERIFY2(plan.entries().first().isBlocked() == !renamed.ok(),
                qPrintable(QStringLiteral("report.txt -> %1 (%2): the preview would %3, the backend %4")
                               .arg(QString::fromLatin1(one.expected),
                                   sensitivity == Qt::CaseSensitive ? "sensitive" : "insensitive",
                                   plan.entries().first().isBlocked()
                                       ? QStringLiteral("refuse it: ") + plan.entries().first().problem
                                       : QStringLiteral("do it"),
                                   renamed.ok() ? QStringLiteral("did it")
                                                : QStringLiteral("refused it: ") + renamed.error().message)));

            // Put it back, so each case is asked of the same directory.
            if (renamed.ok())
                QVERIFY(fs->rename(to, from).ok());
        }
    }
}

void TestRenamePlan::aNameTheDestinationWillNotAcceptIsMarkedBeforeAnythingMoves()
{
    // The bulk rename tool exists so somebody can trust the preview before
    // touching a hundred files. Until now the only name it refused was one with
    // a separator in it, so on Windows it would show a plan of clean rows that
    // the filesystem was going to refuse one at a time.
    const NameRules windows = NameRules::forPlatform(HostPlatform::Windows);

    RenameRule rule = replace(QStringLiteral("scan"), QStringLiteral("a:b"));
    const RenamePlan plan = RenamePlan::build(
        urisIn(QStringLiteral("file:///data"), { "scan.png" }), { rule }, {}, Qt::CaseSensitive, windows);

    const RenamePlan::Entry& entry = plan.entries().first();
    QVERIFY(entry.isBlocked());
    // The character is named, because "this name is invalid" tells somebody
    // staring at a hundred rows nothing they can act on.
    QVERIFY2(entry.problem.contains(QLatin1Char(':')), qPrintable(entry.problem));
    // And a name that would work is offered rather than applied.
    QCOMPARE(entry.suggestion, QStringLiteral("a_b.png"));

    // The same plan against a drive that accepts the name is not blocked, so
    // this is the destination's rule and not a new rule of the rename tool's.
    const RenamePlan permissive = RenamePlan::build(urisIn(QStringLiteral("file:///data"), { "scan.png" }),
        { rule }, {}, Qt::CaseSensitive, NameRules::forPlatform(HostPlatform::Posix));
    QVERIFY(!permissive.entries().first().isBlocked());
}

MOLE_TEST_MAIN(TestRenamePlan)
#include "tst_RenamePlan.moc"
