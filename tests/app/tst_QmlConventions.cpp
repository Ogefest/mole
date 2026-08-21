#include "support/MoleTestMain.h"

#include <QDirIterator>
#include <QFile>
#include <QTest>

/// Rules about the QML that are cheaper to state than to enforce by review.
///
/// Each one here is a fault that was found once and is easy to reintroduce,
/// because the wrong version works perfectly on Linux. A test that reads the
/// source is a blunt instrument, and it is the right one for a claim of the
/// shape "no file does X": it goes on being true for files nobody has written
/// yet.
class TestQmlConventions : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void nothingNamesAFontFamilyByHand();

private:
    /// path -> contents, for every .qml shipped with the application.
    QHash<QString, QString> m_sources;
};

void TestQmlConventions::initTestCase()
{
    QDirIterator it(QStringLiteral(MOLE_QML_SOURCE_DIR), { QStringLiteral("*.qml") }, QDir::Files,
        QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
        m_sources.insert(QDir(QStringLiteral(MOLE_QML_SOURCE_DIR)).relativeFilePath(path),
            QString::fromUtf8(file.readAll()));
    }

    // A scan that found nothing would pass every rule below without reading a
    // line, which is the one way a test like this fails silently.
    QVERIFY2(m_sources.size() > 10,
        qPrintable(QStringLiteral("only %1 qml files found under %2")
                       .arg(m_sources.size())
                       .arg(QStringLiteral(MOLE_QML_SOURCE_DIR))));
}

void TestQmlConventions::nothingNamesAFontFamilyByHand()
{
    // "monospace" is a fontconfig alias. It resolves to a real family on Linux
    // and to nothing on Windows or macOS, where the label silently falls back to
    // the default proportional font and a column of shortcuts stops lining up.
    //
    // AppController::monospaceFont() is the question to ask instead: it walks a
    // preferred list that has a real answer on all three systems, and a reason
    // for preferring one -- the fonts people install for code tell 0 from O and
    // 1 from l, which is the whole point of using one.
    QStringList offenders;
    for (auto it = m_sources.constBegin(); it != m_sources.constEnd(); ++it) {
        const QStringList lines = it.value().split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            static const QRegularExpression named(QStringLiteral("font\\.family\\s*:\\s*\""));
            if (lines.at(i).contains(named))
                offenders.append(QStringLiteral("%1:%2").arg(it.key()).arg(i + 1));
        }
    }

    std::sort(offenders.begin(), offenders.end());
    QVERIFY2(offenders.isEmpty(),
        qPrintable(QStringLiteral("a font family is named by hand at %1 -- ask App.monospaceFont")
                       .arg(offenders.join(QStringLiteral(", ")))));
}

MOLE_TEST_MAIN(TestQmlConventions)
#include "tst_QmlConventions.moc"
