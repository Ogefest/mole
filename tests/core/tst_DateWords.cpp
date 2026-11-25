#include "support/MoleTestMain.h"

#include "core/text/DateWords.h"

#include <QLocale>
#include <QTimeZone>

using namespace mole;

/// A date shown to a person reads the same in every locale.
///
/// The locale is set by each case rather than taken from the machine. The fault
/// this guards against, a two-digit year from `QLocale::ShortFormat`, only ever
/// showed on a workstation running `en_US`, because CI's C locale happened to
/// keep four digits. See MOLE-467.
class TestDateWords : public QObject
{
    Q_OBJECT

private slots:
    void cleanup();

    void aMomentIsYearMonthDayInEveryLocale_data();
    void aMomentIsYearMonthDayInEveryLocale();
    void aDayAndATimeOfDayFollowTheSameForm_data();
    void aDayAndATimeOfDayFollowTheSameForm();
    void aMomentIsShownInLocalTime();
    void nothingIsShownForAnInvalidDate();
};

void TestDateWords::cleanup()
{
    QLocale::setDefault(QLocale::c());
}

void TestDateWords::aMomentIsYearMonthDayInEveryLocale_data()
{
    QTest::addColumn<QString>("locale");
    QTest::addColumn<QDateTime>("when");
    QTest::addColumn<QString>("expected");

    const QDateTime morning(QDate(2026, 3, 14), QTime(9, 31, 7));
    const QDateTime evening(QDate(2026, 11, 5), QTime(21, 5));
    // A century earlier, which the short format made indistinguishable from 2026.
    const QDateTime longAgo(QDate(1926, 3, 14), QTime(9, 31));
    for (const char* name : { "en_US", "de_DE", "C" }) {
        QTest::addRow("%s, morning", name) << name << morning << "2026-03-14 09:31";
        // 24-hour, so no AM or PM in any language.
        QTest::addRow("%s, evening", name) << name << evening << "2026-11-05 21:05";
        QTest::addRow("%s, before 2000", name) << name << longAgo << "1926-03-14 09:31";
    }
}

void TestDateWords::aMomentIsYearMonthDayInEveryLocale()
{
    QFETCH(QString, locale);
    QFETCH(QDateTime, when);
    QFETCH(QString, expected);

    QLocale::setDefault(QLocale(locale));
    QCOMPARE(dateTimeInWords(when), expected);
}

void TestDateWords::aDayAndATimeOfDayFollowTheSameForm_data()
{
    QTest::addColumn<QString>("locale");
    for (const char* name : { "en_US", "de_DE", "C" })
        QTest::newRow(name) << name;
}

void TestDateWords::aDayAndATimeOfDayFollowTheSameForm()
{
    QFETCH(QString, locale);

    QLocale::setDefault(QLocale(locale));
    QCOMPARE(dateInWords(QDate(1999, 12, 31)), QStringLiteral("1999-12-31"));
    QCOMPARE(clockInWords(QTime(7, 4)), QStringLiteral("07:04"));
    QCOMPARE(clockInWords(QTime(19, 45)), QStringLiteral("19:45"));
}

void TestDateWords::aMomentIsShownInLocalTime()
{
    // A document stores its dates in UTC. The person reading it is somewhere, so
    // what they see is that moment on their own clock, whichever zone it was
    // written in.
    const QDateTime written(QDate(2026, 3, 14), QTime(9, 31), QTimeZone::utc());
    const QDateTime here = written.toLocalTime();
    QCOMPARE(dateTimeInWords(written), here.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    QCOMPARE(dateTimeInWords(written), dateTimeInWords(here));
}

void TestDateWords::nothingIsShownForAnInvalidDate()
{
    QVERIFY(dateTimeInWords(QDateTime()).isEmpty());
    QVERIFY(dateInWords(QDate()).isEmpty());
    QVERIFY(clockInWords(QTime()).isEmpty());
}

MOLE_TEST_MAIN(TestDateWords)
#include "tst_DateWords.moc"
