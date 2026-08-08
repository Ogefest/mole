#include "support/MoleTestMain.h"
#include "support/TestSupport.h"

#include "core/settings/Preferences.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

/// The small things the application remembers about how someone likes to work.
class TestPreferences : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void nothingRememberedYetIsNotAFailure();
    void aValueSurvivesBeingReloaded();
    void settingWhatIsAlreadyThereChangesNothing();
    void removingForgetsIt();
    void unreadableFileLeavesItEmptyRatherThanCrashing();
    void theEnvironmentDecidesWhereItLives();

private:
    QString path() const { return QDir(m_dir->path()).filePath(QStringLiteral("preferences.json")); }

    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestPreferences::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
}

void TestPreferences::cleanup()
{
    m_dir.reset();
}

void TestPreferences::nothingRememberedYetIsNotAFailure()
{
    // A first run has no file, which is the normal state and not an error.
    Preferences preferences(path());
    QVERIFY(preferences.load());
    QVERIFY(!preferences.contains(QStringLiteral("preview.text.html.mode")));
    QCOMPARE(preferences.value(QStringLiteral("preview.text.html.mode"), QStringLiteral("Source")).toString(),
        QStringLiteral("Source"));
}

void TestPreferences::aValueSurvivesBeingReloaded()
{
    {
        Preferences preferences(path());
        preferences.setValue(QStringLiteral("preview.text.html.mode"), QStringLiteral("Rendered"));
        preferences.setValue(QStringLiteral("preview.text.xml.mode"), QStringLiteral("Source"));
    }
    QVERIFY2(QFile::exists(path()), "setting a value writes it out, without being asked to save");

    // A fresh instance, as the next run of the application would be.
    Preferences again(path());
    QCOMPARE(again.value(QStringLiteral("preview.text.html.mode")).toString(), QStringLiteral("Rendered"));
    // Keyed per type, so choosing for one does not answer for another.
    QCOMPARE(again.value(QStringLiteral("preview.text.xml.mode")).toString(), QStringLiteral("Source"));
}

void TestPreferences::settingWhatIsAlreadyThereChangesNothing()
{
    Preferences preferences(path());
    QSignalSpy changed(&preferences, &Preferences::changed);

    preferences.setValue(QStringLiteral("a.key"), QStringLiteral("one"));
    QCOMPARE(changed.count(), 1);

    // A view that assigns on every change must not rewrite the file each time.
    preferences.setValue(QStringLiteral("a.key"), QStringLiteral("one"));
    QCOMPARE(changed.count(), 1);

    preferences.setValue(QStringLiteral("a.key"), QStringLiteral("two"));
    QCOMPARE(changed.count(), 2);
}

void TestPreferences::removingForgetsIt()
{
    Preferences preferences(path());
    preferences.setValue(QStringLiteral("a.key"), QStringLiteral("one"));
    QVERIFY(preferences.contains(QStringLiteral("a.key")));

    preferences.remove(QStringLiteral("a.key"));
    QVERIFY(!preferences.contains(QStringLiteral("a.key")));

    // And it stays forgotten, rather than coming back from the file.
    Preferences again(path());
    QVERIFY(!again.contains(QStringLiteral("a.key")));

    // Removing what is not there is not an event.
    QSignalSpy changed(&again, &Preferences::changed);
    again.remove(QStringLiteral("never.existed"));
    QCOMPARE(changed.count(), 0);
}

void TestPreferences::unreadableFileLeavesItEmptyRatherThanCrashing()
{
    QFile file(path());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ this is not json at all");
    file.close();

    // Nonsense on disk means nothing is remembered, not a refusal to start: a
    // corrupted preferences file must never be the reason an application will not
    // open.
    Preferences preferences(path());
    QVERIFY(!preferences.contains(QStringLiteral("anything")));
    QCOMPARE(preferences.value(QStringLiteral("anything"), QStringLiteral("fallback")).toString(),
        QStringLiteral("fallback"));

    // And it can be written over.
    preferences.setValue(QStringLiteral("anything"), QStringLiteral("now"));
    Preferences again(path());
    QCOMPARE(again.value(QStringLiteral("anything")).toString(), QStringLiteral("now"));
}

void TestPreferences::theEnvironmentDecidesWhereItLives()
{
    // Every store here honours an override so tests never touch the developer's
    // own settings; this one is no exception.
    const QString wanted = QDir(m_dir->path()).filePath(QStringLiteral("elsewhere.json"));
    qputenv("MOLE_PREFERENCES_PATH", wanted.toLocal8Bit());
    QCOMPARE(Preferences::defaultPath(), wanted);
    qunsetenv("MOLE_PREFERENCES_PATH");
    QVERIFY(!Preferences::defaultPath().isEmpty());
}

MOLE_TEST_MAIN(TestPreferences)
#include "tst_Preferences.moc"
