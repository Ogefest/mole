#include "support/MoleTestMain.h"

#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
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
    void nothingNamesAColourByHand();
    void nothingBuildsOrTakesApartAUriByHand();
    void nothingInTheShellNamesADriveOrAFilesystem();

private:
    /// path -> contents, for every .qml shipped with the application.
    QHash<QString, QString> m_sources;
    /// The same for the shell's own source: src/ui and src/app, markup and C++
    /// alike. A rule about what the interface may know is not a rule about QML.
    QHash<QString, QString> m_shellSources;
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

    for (const QString& layer : { QStringLiteral("ui"), QStringLiteral("app") }) {
        const QString root = QStringLiteral(MOLE_SHELL_SOURCE_DIR) + QLatin1Char('/') + layer;
        QDirIterator files(root, { QStringLiteral("*.qml"), QStringLiteral("*.cpp"), QStringLiteral("*.h") },
            QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            QFile file(path);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
            m_shellSources.insert(QDir(QStringLiteral(MOLE_SHELL_SOURCE_DIR)).relativeFilePath(path),
                QString::fromUtf8(file.readAll()));
        }
    }

    QVERIFY2(m_shellSources.size() > 40,
        qPrintable(QStringLiteral("only %1 shell sources found under %2")
                       .arg(m_shellSources.size())
                       .arg(QStringLiteral(MOLE_SHELL_SOURCE_DIR))));
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

void TestQmlConventions::nothingNamesAColourByHand()
{
    // There used to be 372 of these across 39 files, 75 distinct values between
    // them, and three separate families of grey that nobody had chosen: they
    // arrived one view at a time and the next view added a fourth. Changing what
    // Mole looks like meant editing every file that had an opinion.
    //
    // `App.colour` is the sixteen tokens to name instead -- window, panel, pane,
    // border, hover, selection, text, textSecondary, textMuted, textFaint, accent,
    // link, ok, warn, bad, busy -- and a theme repaints all of them at once. See
    // ADR-0072.
    //
    // A hex value is what this looks for, and only that. `"transparent"`, `"white"`
    // for a sheet of paper in a document preview, and `Qt.rgba()` for a veil that
    // darkens whatever is behind it are all still honest answers: none of them is
    // a colour of the window's that somebody wrote down twice.
    QStringList offenders;
    for (auto it = m_sources.constBegin(); it != m_sources.constEnd(); ++it) {
        const QStringList lines = it.value().split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            static const QRegularExpression literal(QStringLiteral("\"#[0-9a-fA-F]{6,8}\""));
            if (lines.at(i).contains(literal))
                offenders.append(QStringLiteral("%1:%2").arg(it.key()).arg(i + 1));
        }
    }

    std::sort(offenders.begin(), offenders.end());
    QVERIFY2(offenders.isEmpty(),
        qPrintable(QStringLiteral("a colour is written out at %1 -- name a token on App.colour")
                       .arg(offenders.join(QStringLiteral(", ")))));
}

void TestQmlConventions::nothingBuildsOrTakesApartAUriByHand()
{
    // Three files used to do this, slightly differently each, and none of the
    // three survived a path with a drive letter in it. "file://" + value turns
    // C:\Users\me into a uri whose authority is "C:" and whose path is a run of
    // backslashes; substring(7) turns file:///C:/x into /C:/x.
    //
    // All three worked on Linux, which is why they were there and why a review
    // would not have caught the fourth. App.uriForPathText() and
    // App.pathTextFor() are the pair to ask, and being C++ they follow the uri
    // type instead of having to be found and fixed again.
    struct Forbidden
    {
        const char* pattern;
        const char* insteadUse;
    };
    static const Forbidden rules[] = {
        { "\"file://\"\\s*\\+", "App.uriForPathText()" },
        { "substring\\(7\\)", "App.pathTextFor()" },
        { "indexOf\\(\"://\"\\)", "App.uriForPathText()" },
        { "startsWith\\(\"file://\"\\)", "App.pathTextFor()" },
    };

    QStringList offenders;
    for (const Forbidden& rule : rules) {
        const QRegularExpression forbidden(QString::fromLatin1(rule.pattern));
        QVERIFY2(forbidden.isValid(), rule.pattern);

        for (auto it = m_sources.constBegin(); it != m_sources.constEnd(); ++it) {
            const QStringList lines = it.value().split(QLatin1Char('\n'));
            for (int i = 0; i < lines.size(); ++i) {
                if (!lines.at(i).contains(forbidden))
                    continue;
                offenders.append(QStringLiteral("%1:%2 -- use %3")
                                     .arg(it.key())
                                     .arg(i + 1)
                                     .arg(QString::fromLatin1(rule.insteadUse)));
            }
        }
    }

    std::sort(offenders.begin(), offenders.end());
    QVERIFY2(offenders.isEmpty(),
        qPrintable(QStringLiteral("a uri is built or taken apart by hand:\n  %1")
                       .arg(offenders.join(QStringLiteral("\n  ")))));
}

/// The rule that keeps the shell a shell.
///
/// Everything a drive can do that another cannot arrives as an id, a title and
/// one of two kinds of answer -- so nothing between the backend and the menu has
/// any business naming a filesystem, a storage service or a feature of one. The
/// moment it does, the extension point has stopped being one: the next drive
/// with something to offer needs an edit here, and the one after that needs
/// another. See ADR-0075.
///
/// Comments are stripped before matching. Explaining *why* a rule exists by
/// naming the case it came from is what a comment is for, and half the files
/// here do it.
void TestQmlConventions::nothingInTheShellNamesADriveOrAFilesystem()
{
    static const char* names[] = { "zfs", "btrfs", "apfs", "ntfs", "sftp", "webdav", "smb", "nfs", "s3",
        "ftp", "buckets?", "presign", "shadow copy" };

    QStringList offenders;
    for (const char* name : names) {
        const QRegularExpression forbidden(QStringLiteral("\\b%1\\b").arg(QString::fromLatin1(name)),
            QRegularExpression::CaseInsensitiveOption);
        QVERIFY2(forbidden.isValid(), name);

        for (auto it = m_shellSources.constBegin(); it != m_shellSources.constEnd(); ++it) {
            const QStringList lines = it.value().split(QLatin1Char('\n'));
            for (int i = 0; i < lines.size(); ++i) {
                QString code = lines.at(i);
                const int comment = code.indexOf(QLatin1String("//"));
                if (comment >= 0)
                    code.truncate(comment);
                if (code.contains(forbidden)) {
                    offenders.append(QStringLiteral("%1:%2 names %3")
                                         .arg(it.key())
                                         .arg(i + 1)
                                         .arg(QString::fromLatin1(name)));
                }
            }
        }
    }

    std::sort(offenders.begin(), offenders.end());
    QVERIFY2(offenders.isEmpty(),
        qPrintable(QStringLiteral("the shell names a drive: %1").arg(offenders.join(QStringLiteral(", ")))));
}

MOLE_TEST_MAIN(TestQmlConventions)
#include "tst_QmlConventions.moc"
