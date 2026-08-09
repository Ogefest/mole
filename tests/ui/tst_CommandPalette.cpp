#include "host/ActionRegistry.h"
#include "support/MoleTestMain.h"
#include "support/TestSupport.h"
#include "ui/models/BookmarkModel.h"
#include "ui/models/CommandPaletteModel.h"
#include "ui/models/DriveListModel.h"

#include "core/credentials/SecretStore.h"
#include "core/vfs/RemoteRegistry.h"
#include "core/vfs/VfsManager.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace mole;
using namespace mole::test;

namespace {

MenuAction makeAction(const QString& id, MenuAction::Section section, const QString& title,
    const QString& shortcut = {}, std::function<bool()> enabled = {})
{
    MenuAction action;
    action.id = id;
    action.section = section;
    action.title = title;
    action.shortcut = shortcut;
    action.trigger = [] {};
    action.enabled = std::move(enabled);
    return action;
}

QStringList pathsOf(const CommandPaletteModel& palette)
{
    QStringList out;
    for (int row = 0; row < palette.rowCount(); ++row)
        out.append(palette.data(palette.index(row, 0), CommandPaletteModel::PathRole).toString());
    return out;
}

QStringList titlesOf(const CommandPaletteModel& palette)
{
    QStringList out;
    for (int row = 0; row < palette.rowCount(); ++row)
        out.append(palette.data(palette.index(row, 0), CommandPaletteModel::TitleRole).toString());
    return out;
}

} // namespace

/// One input that can reach everything: the menu, the bookmarks, the drives.
class TestCommandPalette : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void holdsEveryMenuEntryTheMenuWouldShow();
    void aGreyedOutActionIsNotOffered();
    void filteringByPartOfATitleFindsIt();
    void aTitleMatchOutranksAGroupMatch();
    void severalWordsMatchInAnyOrder();
    void bookmarksAreInTheSameList();
    void choosingAMenuEntryAsksForThatAction();
    void choosingABookmarkAsksForThatPlace();
    void refreshingPicksUpWhatHasChangedSince();

    void aDriveOffersWhatCanBeDoneToItAndNotOnlyWhereItGoes();
    void theVerbFollowsTheDriveRatherThanASecondList();
    void aLockedDriveIsOfferedThePassphraseRatherThanAFailure();
    void aLocalDiskIsAPlaceAndNothingElse();

private:
    /// A drive list with a registry behind it, so the palette sees the same
    /// rows the sidebar does rather than a fixture of its own.
    void buildDrives();
    QString configure(const QString& name, bool withSecret = false);
    void connectConfigured(const QString& driveId);

    std::unique_ptr<ActionRegistry> m_actions;
    std::unique_ptr<BookmarkModel> m_bookmarks;
    std::unique_ptr<QTemporaryDir> m_profile;
    std::unique_ptr<SecretStore> m_secrets;
    std::unique_ptr<RemoteRegistry> m_registry;
    std::unique_ptr<VfsManager> m_vfs;
    std::unique_ptr<DriveListModel> m_drives;
};

void TestCommandPalette::buildDrives()
{
    m_secrets
        = std::make_unique<SecretStore>(QDir(m_profile->path()).filePath(QStringLiteral("credentials.enc")));
    m_registry = std::make_unique<RemoteRegistry>(
        QDir(m_profile->path()).filePath(QStringLiteral("drives.json")), m_secrets.get());
    m_vfs = std::make_unique<VfsManager>();
    m_drives = std::make_unique<DriveListModel>(m_vfs.get(), m_registry.get());
}

QString TestCommandPalette::configure(const QString& name, bool withSecret)
{
    RemoteDrive drive;
    drive.name = name;
    drive.factoryScheme = QStringLiteral("sftp");
    drive.variant = QStringLiteral("sftp");
    drive.root = QStringLiteral("/data");
    if (withSecret)
        drive.secretFields.append(QStringLiteral("pass"));

    QVariantMap secretValues;
    if (withSecret)
        secretValues.insert(QStringLiteral("pass"), QStringLiteral("not-a-real-password"));

    QString id;
    QString error;
    if (!m_registry->put(drive, secretValues, &error, &id))
        return {};
    return id;
}

void TestCommandPalette::connectConfigured(const QString& driveId)
{
    const RemoteDrive drive = m_registry->drive(driveId);
    Mount mount;
    mount.id = drive.id;
    mount.displayName = drive.name;
    mount.root = drive.rootUri();
    mount.fileSystem = std::make_shared<MemoryFileSystem>();
    m_vfs->addMount(mount);
}

void TestCommandPalette::init()
{
    m_actions = std::make_unique<ActionRegistry>();
    m_profile = std::make_unique<QTemporaryDir>();
    QVERIFY(m_profile->isValid());
    m_bookmarks
        = std::make_unique<BookmarkModel>(QDir(m_profile->path()).filePath(QStringLiteral("bookmarks.json")));

    m_actions->addAction(makeAction(QStringLiteral("mole.tools.terminal"), MenuAction::Section::Operations,
        QStringLiteral("Terminal here"), QStringLiteral("Ctrl+`")));
    m_actions->addAction(makeAction(QStringLiteral("mole.tools.addToSet"), MenuAction::Section::Operations,
        QStringLiteral("Add to set")));
    m_actions->addAction(makeAction(QStringLiteral("mole.tools.bulkRename"), MenuAction::Section::Workflows,
        QStringLiteral("Bulk rename")));
    m_actions->addAction(makeAction(
        QStringLiteral("mole.view.refresh"), MenuAction::Section::View, QStringLiteral("Refresh")));
}

void TestCommandPalette::cleanup()
{
    m_drives.reset();
    m_vfs.reset();
    m_registry.reset();
    m_secrets.reset();
    m_bookmarks.reset();
    m_profile.reset();
    m_actions.reset();
}

void TestCommandPalette::holdsEveryMenuEntryTheMenuWouldShow()
{
    // The assertion this feature stands on. The palette is a view over the
    // registries, so anything the menu offers it offers -- if this can drift, the
    // palette becomes a second menu maintained by hand and stops being trustworthy.
    CommandPaletteModel palette(m_actions.get(), nullptr, nullptr);
    palette.refresh();

    QStringList fromMenu;
    for (const QVariant& sectionEntry : m_actions->buildModel()) {
        const QVariantMap section = sectionEntry.toMap();
        for (const QVariant& action : section.value(QStringLiteral("actions")).toList()) {
            fromMenu.append(section.value(QStringLiteral("title")).toString() + QStringLiteral(" → ")
                + action.toMap().value(QStringLiteral("title")).toString());
        }
    }

    QCOMPARE(pathsOf(palette), fromMenu);

    // And it carries the keys that already do the job, so the palette teaches them
    // rather than replacing them.
    const int terminal = titlesOf(palette).indexOf(QStringLiteral("Terminal here"));
    QVERIFY(terminal >= 0);
    QCOMPARE(palette.data(palette.index(terminal, 0), CommandPaletteModel::ShortcutRole).toString(),
        QStringLiteral("Ctrl+`"));
}

void TestCommandPalette::aGreyedOutActionIsNotOffered()
{
    bool allowed = false;
    m_actions->addAction(makeAction(QStringLiteral("mole.tools.analyse"), MenuAction::Section::Workflows,
        QStringLiteral("Analyse folder"), QString(), [&allowed] { return allowed; }));

    CommandPaletteModel palette(m_actions.get(), nullptr, nullptr);
    palette.refresh();
    QVERIFY2(!titlesOf(palette).contains(QStringLiteral("Analyse folder")),
        "offering something that cannot run is worse than not offering it");

    // The state is read when the palette is opened, not when the action was
    // registered, so it comes back the moment it applies again.
    allowed = true;
    palette.refresh();
    QVERIFY(titlesOf(palette).contains(QStringLiteral("Analyse folder")));
}

void TestCommandPalette::filteringByPartOfATitleFindsIt()
{
    CommandPaletteModel palette(m_actions.get(), nullptr, nullptr);
    palette.refresh();
    const int everything = palette.rowCount();
    QVERIFY(everything >= 4);

    palette.setFilter(QStringLiteral("termi"));
    QCOMPARE(titlesOf(palette), QStringList { QStringLiteral("Terminal here") });
    QCOMPARE(palette.data(palette.index(0, 0), CommandPaletteModel::PathRole).toString(),
        QStringLiteral("Operations → Terminal here"));

    // Case does not matter: nobody types capitals into a search box.
    palette.setFilter(QStringLiteral("TERMINAL"));
    QCOMPARE(palette.rowCount(), 1);

    // And clearing it brings everything back rather than leaving a filtered view.
    palette.setFilter(QString());
    QCOMPARE(palette.rowCount(), everything);
}

void TestCommandPalette::aTitleMatchOutranksAGroupMatch()
{
    // "set" appears in the title of one entry and in the section name "Operations"
    // of another. Without ranking, the one being looked for would be buried.
    CommandPaletteModel palette(m_actions.get(), nullptr, nullptr);
    palette.refresh();
    palette.setFilter(QStringLiteral("set"));

    QVERIFY(palette.rowCount() >= 1);
    QCOMPARE(palette.data(palette.index(0, 0), CommandPaletteModel::TitleRole).toString(),
        QStringLiteral("Add to set"));
}

void TestCommandPalette::severalWordsMatchInAnyOrder()
{
    CommandPaletteModel palette(m_actions.get(), nullptr, nullptr);
    palette.refresh();

    // Typing the group and then the entry is a natural way to ask for something
    // when you remember roughly where it lives.
    palette.setFilter(QStringLiteral("op term"));
    QCOMPARE(titlesOf(palette), QStringList { QStringLiteral("Terminal here") });

    palette.setFilter(QStringLiteral("term op"));
    QCOMPARE(titlesOf(palette), QStringList { QStringLiteral("Terminal here") });

    palette.setFilter(QStringLiteral("op nothinglikethis"));
    QCOMPARE(palette.rowCount(), 0);
}

void TestCommandPalette::bookmarksAreInTheSameList()
{
    QVERIFY(m_bookmarks->add(QStringLiteral("file:///tmp/photos"), QStringLiteral("Holiday photos")));

    CommandPaletteModel palette(m_actions.get(), m_bookmarks.get(), nullptr);
    palette.refresh();

    QVERIFY(pathsOf(palette).contains(QStringLiteral("Bookmarks → Holiday photos")));

    // Filtering reaches them the same way it reaches the menu: one box, one list.
    palette.setFilter(QStringLiteral("holi"));
    QCOMPARE(titlesOf(palette), QStringList { QStringLiteral("Holiday photos") });
}

void TestCommandPalette::choosingAMenuEntryAsksForThatAction()
{
    CommandPaletteModel palette(m_actions.get(), nullptr, nullptr);
    palette.refresh();
    palette.setFilter(QStringLiteral("bulk"));
    QCOMPARE(palette.rowCount(), 1);

    // The model asks; the shell acts. That is what lets it stay a plain view over
    // the registries and know nothing about tabs.
    QSignalSpy chosen(&palette, &CommandPaletteModel::actionRequested);
    palette.activate(0);
    QCOMPARE(chosen.count(), 1);
    QCOMPARE(chosen.first().first().toString(), QStringLiteral("mole.tools.bulkRename"));

    // A row that is not there does nothing at all.
    palette.activate(42);
    QCOMPARE(chosen.count(), 1);
}

void TestCommandPalette::choosingABookmarkAsksForThatPlace()
{
    QVERIFY(m_bookmarks->add(QStringLiteral("file:///tmp/photos"), QStringLiteral("Holiday photos")));

    CommandPaletteModel palette(m_actions.get(), m_bookmarks.get(), nullptr);
    palette.refresh();
    palette.setFilter(QStringLiteral("holiday"));
    QCOMPARE(palette.rowCount(), 1);

    QSignalSpy places(&palette, &CommandPaletteModel::locationRequested);
    QSignalSpy actions(&palette, &CommandPaletteModel::actionRequested);
    palette.activate(0);

    QCOMPARE(places.count(), 1);
    QCOMPARE(places.first().first().toString(), QStringLiteral("file:///tmp/photos"));
    QCOMPARE(actions.count(), 0);
}

void TestCommandPalette::refreshingPicksUpWhatHasChangedSince()
{
    CommandPaletteModel palette(m_actions.get(), m_bookmarks.get(), nullptr);
    palette.refresh();
    const int before = palette.rowCount();

    // A bookmark added while the palette existed, and an action registered by a
    // plugin loaded later, both have to appear the next time it opens.
    QVERIFY(m_bookmarks->add(QStringLiteral("file:///tmp/work"), QStringLiteral("Work")));
    m_actions->addAction(makeAction(QStringLiteral("org.example.thing"), MenuAction::Section::Workflows,
        QStringLiteral("A plugin thing")));

    palette.refresh();
    QCOMPARE(palette.rowCount(), before + 2);
    QVERIFY(titlesOf(palette).contains(QStringLiteral("A plugin thing")));
    QVERIFY(titlesOf(palette).contains(QStringLiteral("Work")));
}

// ----------------------------------------------- doing things to a drive

/// A drive you can only connect by finding a small button with the pointer is
/// half-built in an application whose palette is described as the one key that
/// reaches everything. What the palette offered for a drive was somewhere to
/// go; it now also offers what can be done to it.
void TestCommandPalette::aDriveOffersWhatCanBeDoneToItAndNotOnlyWhereItGoes()
{
    buildDrives();
    QVERIFY(!configure(QStringLiteral("Office NAS")).isEmpty());

    CommandPaletteModel palette(nullptr, nullptr, m_drives.get());
    palette.refresh();

    const QStringList titles = titlesOf(palette);
    QVERIFY2(titles.contains(QStringLiteral("Office NAS")), "somewhere to go, as before");
    QVERIFY(titles.contains(QStringLiteral("Connect Office NAS")));
    QVERIFY(titles.contains(QStringLiteral("Check Office NAS")));
    QVERIFY2(!titles.contains(QStringLiteral("Eject Office NAS")),
        "nothing to eject: offering it would be offering something that cannot run");
}

/// The entries are read from the drive list every time, so a drive that has
/// just connected offers Eject and not Connect without anything keeping a
/// second list of verbs in step -- the same reason the palette reads the menu
/// rather than copying it.
void TestCommandPalette::theVerbFollowsTheDriveRatherThanASecondList()
{
    buildDrives();
    const QString id = configure(QStringLiteral("Office NAS"));
    QVERIFY(!id.isEmpty());

    CommandPaletteModel palette(nullptr, nullptr, m_drives.get());
    palette.refresh();
    QVERIFY(titlesOf(palette).contains(QStringLiteral("Connect Office NAS")));

    connectConfigured(id);
    palette.refresh();

    const QStringList titles = titlesOf(palette);
    QVERIFY(titles.contains(QStringLiteral("Eject Office NAS")));
    QVERIFY(!titles.contains(QStringLiteral("Connect Office NAS")));
    QVERIFY2(titles.contains(QStringLiteral("Check Office NAS")),
        "is it actually there is a fair question whatever the row claims");

    // Choosing it asks for the same thing the sidebar button asks for, by id,
    // rather than this model knowing how to eject anything.
    QSignalSpy asked(&palette, &CommandPaletteModel::driveCommandRequested);
    palette.setFilter(QStringLiteral("Eject Office"));
    QVERIFY(palette.rowCount() > 0);
    palette.activate(0);

    QCOMPARE(asked.count(), 1);
    QCOMPARE(asked.first().at(0).value<CommandPaletteModel::DriveCommand>(),
        CommandPaletteModel::DriveCommand::Eject);
    QCOMPARE(asked.first().at(1).toString(), id);
}

/// "Connect" would fail every time, and silence would be worse. The entry names
/// the thing that is actually in the way.
void TestCommandPalette::aLockedDriveIsOfferedThePassphraseRatherThanAFailure()
{
    if (!SecretStore::isAvailable())
        QSKIP("this build cannot encrypt");

    buildDrives();
    QVERIFY(m_secrets->create(QStringLiteral("phrase")));
    const QString id = configure(QStringLiteral("Office NAS"), true);
    QVERIFY(!id.isEmpty());
    m_secrets->lock();

    CommandPaletteModel palette(nullptr, nullptr, m_drives.get());
    palette.refresh();

    const QStringList titles = titlesOf(palette);
    QVERIFY(titles.contains(QStringLiteral("Unlock the credential store for Office NAS")));
    QVERIFY2(!titles.contains(QStringLiteral("Connect Office NAS")),
        "offering connect here would fail every time instead of saying what is wrong");

    QSignalSpy asked(&palette, &CommandPaletteModel::driveCommandRequested);
    palette.setFilter(QStringLiteral("Unlock"));
    QVERIFY(palette.rowCount() > 0);
    palette.activate(0);
    QCOMPARE(asked.count(), 1);
    QCOMPARE(asked.first().at(0).value<CommandPaletteModel::DriveCommand>(),
        CommandPaletteModel::DriveCommand::Unlock);
}

/// A local disk is somewhere to go and nothing else: there is no connecting or
/// ejecting to be done to it, and no check that being there does not answer.
void TestCommandPalette::aLocalDiskIsAPlaceAndNothingElse()
{
    buildDrives();
    Mount mount;
    mount.id = QStringLiteral("scratch");
    mount.displayName = QStringLiteral("Scratch");
    mount.root = VfsUri::fromString(QStringLiteral("mem://scratch/"));
    mount.fileSystem = std::make_shared<MemoryFileSystem>();
    m_vfs->addMount(mount);

    CommandPaletteModel palette(nullptr, nullptr, m_drives.get());
    palette.refresh();

    const QStringList titles = titlesOf(palette);
    QCOMPARE(titles, QStringList { QStringLiteral("Scratch") });
}

MOLE_TEST_MAIN(TestCommandPalette)
#include "tst_CommandPalette.moc"
