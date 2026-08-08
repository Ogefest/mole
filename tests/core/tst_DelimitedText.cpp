#include "support/MoleTestMain.h"

#include "core/text/DelimitedText.h"

using namespace mole;

class TestDelimitedText : public QObject
{
    Q_OBJECT

private slots:
    void detectsSeparator_data();
    void detectsSeparator();

    void parsesASimpleTable();
    void honoursAnExplicitSeparator();
    void handlesQuotedFields();
    void handlesDoubledQuotes();
    void handlesNewlinesInsideQuotes();
    void handlesCrlf();
    void padsRaggedRows();
    void treatsFirstRowAsDataWhenAsked();
    void stopsAtTheRowLimit();
    void emptyInputYieldsAnEmptyTable();
    void survivesAnUnterminatedQuote();
    void keepsEmptyFields();
};

void TestDelimitedText::detectsSeparator_data()
{
    QTest::addColumn<QString>("sample");
    QTest::addColumn<QChar>("expected");

    QTest::newRow("comma") << "a,b,c\n1,2,3\n" << QChar(',');
    QTest::newRow("tab") << "a\tb\tc\n1\t2\t3\n" << QChar('\t');
    QTest::newRow("semicolon") << "a;b;c\n1;2;3\n" << QChar(';');
    QTest::newRow("pipe") << "a|b|c\n1|2|3\n" << QChar('|');

    // A European export: semicolons separate, commas are decimal points. The
    // naive "count the commas" answer is wrong here.
    QTest::newRow("semicolon with decimal commas") << "name;price;qty\nwidget;1,50;3\nbolt;0,99;10\n"
                                                   << QChar(';');

    // Commas inside quoted prose must not win over the real separator.
    QTest::newRow("tabs with prose") << "id\tnote\n1\t\"one, two, three\"\n2\t\"four, five\"\n"
                                     << QChar('\t');

    QTest::newRow("single column falls back to comma") << "just\nsome\nlines\n" << QChar(',');
}

void TestDelimitedText::detectsSeparator()
{
    QFETCH(QString, sample);
    QFETCH(QChar, expected);
    QCOMPARE(DelimitedTextParser::detectSeparator(sample), expected);
}

void TestDelimitedText::parsesASimpleTable()
{
    const DelimitedTable table = DelimitedTextParser::parse(QStringLiteral("name,age\nAda,36\nAlan,41\n"));

    QCOMPARE(table.separator, QChar(','));
    QCOMPARE(table.headers, QStringList({ QStringLiteral("name"), QStringLiteral("age") }));
    QCOMPARE(table.rows.size(), 2);
    QCOMPARE(table.rows.at(0), QStringList({ QStringLiteral("Ada"), QStringLiteral("36") }));
    QCOMPARE(table.rows.at(1).at(0), QStringLiteral("Alan"));
    QCOMPARE(table.columnCount(), 2);
    QVERIFY(!table.truncated);
}

void TestDelimitedText::honoursAnExplicitSeparator()
{
    // The user overriding detection is the whole point of exposing the option.
    DelimitedTextParser::Options options;
    options.separator = QLatin1Char(';');

    const DelimitedTable table = DelimitedTextParser::parse(QStringLiteral("a,b;c,d\n1,2;3,4\n"), options);

    QCOMPARE(table.separator, QChar(';'));
    QCOMPARE(table.headers, QStringList({ QStringLiteral("a,b"), QStringLiteral("c,d") }));
}

void TestDelimitedText::handlesQuotedFields()
{
    const DelimitedTable table = DelimitedTextParser::parse(QStringLiteral("id,note\n1,\"one, two\"\n"));

    QCOMPARE(table.rows.at(0), QStringList({ QStringLiteral("1"), QStringLiteral("one, two") }));
}

void TestDelimitedText::handlesDoubledQuotes()
{
    const DelimitedTable table
        = DelimitedTextParser::parse(QStringLiteral("id,note\n1,\"she said \"\"hello\"\"\"\n"));

    QCOMPARE(table.rows.at(0).at(1), QStringLiteral("she said \"hello\""));
}

void TestDelimitedText::handlesNewlinesInsideQuotes()
{
    // A record and a line are not the same thing, which is the part naive
    // splitters get wrong.
    const DelimitedTable table
        = DelimitedTextParser::parse(QStringLiteral("id,note\n1,\"first\nsecond\"\n2,plain\n"));

    QCOMPARE(table.rows.size(), 2);
    QCOMPARE(table.rows.at(0).at(1), QStringLiteral("first\nsecond"));
    QCOMPARE(table.rows.at(1).at(1), QStringLiteral("plain"));
}

void TestDelimitedText::handlesCrlf()
{
    const DelimitedTable table = DelimitedTextParser::parse(QStringLiteral("a,b\r\n1,2\r\n3,4\r\n"));

    QCOMPARE(table.rows.size(), 2);
    QCOMPARE(table.rows.at(0), QStringList({ QStringLiteral("1"), QStringLiteral("2") }));
    QVERIFY2(!table.rows.at(0).at(1).contains(QLatin1Char('\r')), "the carriage return must go");
}

void TestDelimitedText::padsRaggedRows()
{
    // One short line must not hide the rest of the file.
    const DelimitedTable table = DelimitedTextParser::parse(QStringLiteral("a,b,c\n1,2,3\n4,5\n6,7,8\n"));

    QCOMPARE(table.rows.size(), 3);
    QCOMPARE(table.rows.at(1).size(), 3);
    QCOMPARE(table.rows.at(1).at(2), QString());
}

void TestDelimitedText::treatsFirstRowAsDataWhenAsked()
{
    DelimitedTextParser::Options options;
    options.firstRowIsHeader = false;

    const DelimitedTable table = DelimitedTextParser::parse(QStringLiteral("1,2\n3,4\n"), options);

    QVERIFY(table.headers.isEmpty());
    QCOMPARE(table.rows.size(), 2);
}

void TestDelimitedText::stopsAtTheRowLimit()
{
    QString text = QStringLiteral("a,b\n");
    for (int i = 0; i < 500; ++i)
        text += QStringLiteral("%1,%2\n").arg(i).arg(i * 2);

    DelimitedTextParser::Options options;
    options.maxRows = 100;

    const DelimitedTable table = DelimitedTextParser::parse(text, options);

    // Previewing a huge export must not read all of it, and the caller has to
    // be able to say so.
    QCOMPARE(table.rows.size(), 100);
    QVERIFY(table.truncated);
}

void TestDelimitedText::emptyInputYieldsAnEmptyTable()
{
    const DelimitedTable table = DelimitedTextParser::parse(QString());
    QVERIFY(table.isEmpty());
    QCOMPARE(table.columnCount(), 0);
}

void TestDelimitedText::survivesAnUnterminatedQuote()
{
    // Truncated downloads happen. Producing the partial field beats producing
    // nothing.
    const DelimitedTable table = DelimitedTextParser::parse(QStringLiteral("a,b\n1,\"never closed\n"));

    QCOMPARE(table.rows.size(), 1);
    QVERIFY(table.rows.at(0).at(1).startsWith(QStringLiteral("never closed")));
}

void TestDelimitedText::keepsEmptyFields()
{
    const DelimitedTable table = DelimitedTextParser::parse(QStringLiteral("a,b,c\n1,,3\n"));

    QCOMPARE(table.rows.at(0).size(), 3);
    QCOMPARE(table.rows.at(0).at(1), QString());
}

MOLE_TEST_MAIN(TestDelimitedText)
#include "tst_DelimitedText.moc"
