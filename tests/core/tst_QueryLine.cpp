#include "support/MoleTestMain.h"

#include "core/search/QueryLine.h"

#include <QTest>

using namespace mole;

/// The line above the form, read.
///
/// The syntax is the field names and nothing clever, because the point is that
/// somebody who learnt the form already knows it. Everything is `and`: there is
/// no `or`, no parentheses and no precedence, and that is a decision rather
/// than an omission.
class TestQueryLine : public QObject
{
    Q_OBJECT

private slots:
    void aBareWordIsJustAWord();
    void keysAndOperatorsReadAsWritten();
    void aComparisonMayFollowAColon();
    void quotesHoldASpaceAndSlashesHoldAPattern();
    void aDashNegates();
    void anUnclosedQuoteIsAnErrorWithAPlace();
    void aKeyWithNothingAfterItIsAnError();
    void printingWhatWasParsedGivesTheLineBack();
    void aQuoteInsideAValueSurvivesTheRoundTrip();
    void aDashOnItsOwnIsRefusedRatherThanSearchedFor();

private:
    static QueryTerm only(const QString& line)
    {
        const ParsedQueryLine parsed = parseQueryLine(line);
        return parsed.terms.size() == 1 ? parsed.terms.first() : QueryTerm {};
    }
};

void TestQueryLine::aBareWordIsJustAWord()
{
    const ParsedQueryLine parsed = parseQueryLine(QStringLiteral("report quarterly"));
    QVERIFY(parsed.ok());
    QCOMPARE(parsed.terms.size(), 2);
    QVERIFY(parsed.terms.first().key.isEmpty());
    QCOMPARE(parsed.terms.first().value, QStringLiteral("report"));
    QCOMPARE(parsed.terms.last().value, QStringLiteral("quarterly"));

    // A word with a colon in it is still a word to the parser: whether the left
    // half is a key is the caller's business, so a file really called
    // `notes:2026.txt` stays findable.
    const QueryTerm colon = only(QStringLiteral("notes:2026.txt"));
    QCOMPARE(colon.key, QStringLiteral("notes"));
    QCOMPARE(colon.value, QStringLiteral("2026.txt"));
}

void TestQueryLine::keysAndOperatorsReadAsWritten()
{
    const ParsedQueryLine parsed
        = parseQueryLine(QStringLiteral("report ext:pdf size>10M image.iso>=800 depth<=2"));
    QVERIFY(parsed.ok());
    QCOMPARE(parsed.terms.size(), 5);

    QCOMPARE(parsed.terms.at(1).key, QStringLiteral("ext"));
    QCOMPARE(parsed.terms.at(1).value, QStringLiteral("pdf"));
    QCOMPARE(parsed.terms.at(1).op, QueryTerm::Op::Is);

    QCOMPARE(parsed.terms.at(2).key, QStringLiteral("size"));
    QCOMPARE(parsed.terms.at(2).value, QStringLiteral("10M"));
    QCOMPARE(parsed.terms.at(2).op, QueryTerm::Op::Above);

    // A namespaced key is one key, not a word and a dot and another word.
    QCOMPARE(parsed.terms.at(3).key, QStringLiteral("image.iso"));
    QCOMPARE(parsed.terms.at(3).op, QueryTerm::Op::AtLeast);

    QCOMPARE(parsed.terms.at(4).op, QueryTerm::Op::AtMost);
}

void TestQueryLine::aComparisonMayFollowAColon()
{
    // `modified:<30d` reads better than `modified<30d` and means the same.
    const QueryTerm term = only(QStringLiteral("modified:<30d"));
    QCOMPARE(term.key, QStringLiteral("modified"));
    QCOMPARE(term.value, QStringLiteral("30d"));
    QCOMPARE(term.op, QueryTerm::Op::Below);
}

void TestQueryLine::quotesHoldASpaceAndSlashesHoldAPattern()
{
    const QueryTerm quoted = only(QStringLiteral("content:\"TODO(perf) here\""));
    QCOMPARE(quoted.key, QStringLiteral("content"));
    QCOMPARE(quoted.value, QStringLiteral("TODO(perf) here"));
    QVERIFY(quoted.wasQuoted);
    QVERIFY(!quoted.isRegex);

    // The one place a pattern is guessed at rather than chosen from a control,
    // and it is guessed at only inside slashes.
    const QueryTerm pattern = only(QStringLiteral("name:/^IMG_\\d{4}/"));
    QCOMPARE(pattern.key, QStringLiteral("name"));
    QCOMPARE(pattern.value, QStringLiteral("^IMG_\\d{4}"));
    QVERIFY(pattern.isRegex);
}

void TestQueryLine::aDashNegates()
{
    const QueryTerm term = only(QStringLiteral("-path:node_modules"));
    QVERIFY(term.negate);
    QCOMPARE(term.key, QStringLiteral("path"));
    QCOMPARE(term.value, QStringLiteral("node_modules"));

    // A dash inside a word is part of the word.
    QVERIFY(!only(QStringLiteral("annual-report")).negate);
    QCOMPARE(only(QStringLiteral("annual-report")).value, QStringLiteral("annual-report"));
}

void TestQueryLine::anUnclosedQuoteIsAnErrorWithAPlace()
{
    const ParsedQueryLine parsed = parseQueryLine(QStringLiteral("report content:\"never ends"));
    QVERIFY2(!parsed.ok(), "a query nobody can read must not run");
    QCOMPARE(parsed.errors.size(), 1);
    QCOMPARE(parsed.errors.first().position, 7);
    QVERIFY(parsed.errors.first().length > 0);

    const ParsedQueryLine slashed = parseQueryLine(QStringLiteral("name:/^IMG"));
    QVERIFY(!slashed.ok());
}

void TestQueryLine::aKeyWithNothingAfterItIsAnError()
{
    const ParsedQueryLine parsed = parseQueryLine(QStringLiteral("ext:"));
    QVERIFY(!parsed.ok());
    QVERIFY2(parsed.errors.first().message.contains(QStringLiteral("ext")),
        qPrintable(parsed.errors.first().message));
}

/// Parse, print, parse gives the same thing. The line and the form are one
/// query seen twice, and a round trip that drifts would have them fighting each
/// other while somebody types.
void TestQueryLine::printingWhatWasParsedGivesTheLineBack()
{
    const QStringList lines {
        QStringLiteral("report ext:pdf size>10M"),
        QStringLiteral("holiday type:image image.camera:\"X100V\" image.iso>800"),
        QStringLiteral("ext:cpp,h content:\"TODO(perf)\""),
        QStringLiteral("name:/^IMG_\\d{4}/ -path:node_modules"),
        QStringLiteral("modified<=30d depth:0"),
        // A quote inside a quoted value. This printed back as `content:"say
        // "hi""` and re-parsed as three terms, so a search somebody typed once
        // became a different search the moment the form rewrote the line.
        // See MOLE-405.
        QStringLiteral("content:\"say \\\"hi\\\"\""),
        QStringLiteral("content:\"a backslash \\\\ and a quote \\\"\""),
        // A name that begins with a dash, which is not a negation and printed as
        // if it were.
        QStringLiteral("name:\"-fix\""),
    };

    for (const QString& line : lines) {
        const ParsedQueryLine first = parseQueryLine(line);
        QVERIFY2(first.ok(), qPrintable(line));

        const QString printed = printQueryLine(first.terms);
        const ParsedQueryLine second = parseQueryLine(printed);
        QVERIFY2(second.ok(), qPrintable(printed));
        QCOMPARE(second.terms.size(), first.terms.size());

        for (int i = 0; i < first.terms.size(); ++i) {
            QCOMPARE(second.terms.at(i).key, first.terms.at(i).key);
            QCOMPARE(second.terms.at(i).value, first.terms.at(i).value);
            QCOMPARE(second.terms.at(i).op, first.terms.at(i).op);
            QCOMPARE(second.terms.at(i).negate, first.terms.at(i).negate);
            QCOMPARE(second.terms.at(i).isRegex, first.terms.at(i).isRegex);
        }
        QCOMPARE(printQueryLine(second.terms), printed);
    }
}

/// The escape, stated on its own rather than only inside the round-trip loop.
///
/// The header calls the round trip load-bearing: the form rewrites the line from
/// the fields on every change, so a value the printer cannot represent is a
/// search that silently becomes a different search. See MOLE-405.
void TestQueryLine::aQuoteInsideAValueSurvivesTheRoundTrip()
{
    QueryTerm term;
    term.key = QStringLiteral("content");
    term.value = QStringLiteral("say \"hi\"");
    term.wasQuoted = true;

    const QString printed = printQueryLine({ term });
    QCOMPARE(printed, QStringLiteral("content:\"say \\\"hi\\\"\""));

    const ParsedQueryLine parsed = parseQueryLine(printed);
    QVERIFY2(parsed.ok(), qPrintable(printed));
    QCOMPARE(parsed.terms.size(), 1);
    QCOMPARE(parsed.terms.first().value, QStringLiteral("say \"hi\""));

    // A backslash is itself escaped, or the escape could not be searched for.
    QueryTerm slashed;
    slashed.value = QStringLiteral("C:\\path");
    const ParsedQueryLine back = parseQueryLine(printQueryLine({ slashed }));
    QVERIFY(back.ok());
    QCOMPARE(back.terms.size(), 1);
    QCOMPARE(back.terms.first().value, QStringLiteral("C:\\path"));

    // A single quote closes a single-quoted value and nothing else, so a value
    // holding one needs no escape when the printer used double quotes.
    const ParsedQueryLine single = parseQueryLine(QStringLiteral("content:'it\"s'"));
    QVERIFY(single.ok());
    QCOMPARE(single.terms.first().value, QStringLiteral("it\"s"));
}

/// "A dash on its own matches nothing" used to be unreachable.
///
/// A `-` followed by a space, or at the end of the line, was not read as a
/// negation -- so it fell through to the bare-word branch, became the word `-`,
/// and was searched for. The message existed and no input could produce it.
/// See MOLE-405.
void TestQueryLine::aDashOnItsOwnIsRefusedRatherThanSearchedFor()
{
    for (const QString& line :
        { QStringLiteral("-"), QStringLiteral("report -"), QStringLiteral("- report") }) {
        const ParsedQueryLine parsed = parseQueryLine(line);
        QVERIFY2(!parsed.ok(), qPrintable(line));
        QCOMPARE(parsed.errors.size(), 1);
        QCOMPARE(parsed.errors.first().message, QStringLiteral("A dash on its own matches nothing"));
        QCOMPARE(parsed.errors.first().length, 1);
        QCOMPARE(line.mid(parsed.errors.first().position, 1), QStringLiteral("-"));
    }

    // And there are still two ways to look for the character itself.
    QCOMPARE(only(QStringLiteral("name:-")).value, QStringLiteral("-"));
    QCOMPARE(only(QStringLiteral("\"-\"")).value, QStringLiteral("-"));
    // A dash that negates, and one inside a word, are untouched.
    QVERIFY(only(QStringLiteral("-path:node_modules")).negate);
    QCOMPARE(only(QStringLiteral("annual-report")).value, QStringLiteral("annual-report"));
}

MOLE_TEST_MAIN(TestQueryLine)
#include "tst_QueryLine.moc"
