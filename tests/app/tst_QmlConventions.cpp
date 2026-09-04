#include "support/MoleTestMain.h"

#include <QDirIterator>
#include <QFile>
#include <QKeySequence>
#include <QMetaEnum>
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
    void nothingPaintsWithTheStyleOrADerivation();
    void nothingBuildsOrTakesApartAUriByHand();
    void nothingInTheShellNamesADriveOrAFilesystem();
    void everyKeyTheMenuAdvertisesIsAKeyTheWindowDeclares();
    void noIconOnlyControlIsSmallerThanTheFloor();
    void everyDialogIsBuiltOnTheOneBase();
    void noImageDecodeIsBoundToALiveSize();
    void theMenuHeadingsAreTheOnesTheDocumentsAndTheTooltipsName();

private:
    /// A file under src/, read whole. For the rules that ask about a layer the
    /// two hashes above do not cover.
    static QString readSource(const QString& relativePath);
    /// A document from the top of the repository -- README.md, ARCHITECTURE.md.
    static QString readDocument(const QString& name);
    /// Every C++ source under a directory of src/, joined. For a rule about
    /// something the built-ins spell wherever they happen to live.
    static QString readSourcesUnder(const QString& relativeDirectory);

    /// path -> contents, for every .qml shipped with the application.
    QHash<QString, QString> m_sources;
    /// The same for the shell's own source: src/ui and src/app, markup and C++
    /// alike. A rule about what the interface may know is not a rule about QML.
    QHash<QString, QString> m_shellSources;
};

QString TestQmlConventions::readSource(const QString& relativePath)
{
    QFile file(QStringLiteral(MOLE_SHELL_SOURCE_DIR) + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString TestQmlConventions::readDocument(const QString& name)
{
    // MOLE_SHELL_SOURCE_DIR is the src directory, so the repository is above it.
    QFile file(QDir(QStringLiteral(MOLE_SHELL_SOURCE_DIR)).filePath(QStringLiteral("../") + name));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString TestQmlConventions::readSourcesUnder(const QString& relativeDirectory)
{
    QString joined;
    QDirIterator files(QStringLiteral(MOLE_SHELL_SOURCE_DIR) + QLatin1Char('/') + relativeDirectory,
        { QStringLiteral("*.cpp"), QStringLiteral("*.h") }, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (file.open(QIODevice::ReadOnly))
            joined += QString::fromUtf8(file.readAll());
    }
    return joined;
}

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

    // **And it names no plugin's class, and is not compiled differently because a
    // plugin's library was there.** AppController held seven members behind
    // `#ifdef MOLE_HAVE_ARCHIVE`, each calling a static of the archive plugin's
    // own CompressTask, so the shell knew which plugin writes archives and knew
    // its format table -- the one contribution that did not arrive through the
    // SDK. See ADR-0101 and MOLE-415.
    static const char* plugins[]
        = { "CompressTask", "MOLE_HAVE_ARCHIVE", "ArchiveFileSystem", "plugins/archive", "plugins/network" };
    QStringList named;
    for (const char* symbol : plugins) {
        for (auto it = m_shellSources.constBegin(); it != m_shellSources.constEnd(); ++it) {
            // Only src/ui and src/app: the built-ins are plugins themselves and
            // may name their own parts.
            if (!it.key().startsWith(QStringLiteral("ui/")) && !it.key().startsWith(QStringLiteral("app/")))
                continue;
            const QStringList lines = it.value().split(QLatin1Char('\n'));
            for (int i = 0; i < lines.size(); ++i) {
                QString code = lines.at(i);
                const int comment = code.indexOf(QLatin1String("//"));
                if (comment >= 0)
                    code.truncate(comment);
                if (code.contains(QLatin1String(symbol))) {
                    named.append(QStringLiteral("%1:%2 names %3")
                                     .arg(it.key())
                                     .arg(i + 1)
                                     .arg(QString::fromLatin1(symbol)));
                }
            }
        }
    }
    std::sort(named.begin(), named.end());
    QVERIFY2(named.isEmpty(),
        qPrintable(
            QStringLiteral("the shell knows a plugin by name: %1").arg(named.join(QStringLiteral(", ")))));
}

void TestQmlConventions::everyKeyTheMenuAdvertisesIsAKeyTheWindowDeclares()
{
    // **`MenuAction::shortcut` is display-only**, and Main.qml's own comment says
    // a key named in the menu and not declared there "would be advertised and do
    // nothing". Five were in exactly that state: Ctrl+Shift+A (Analyse),
    // Ctrl+Shift+R (Bulk rename), Ctrl+Shift+L (Alerts) and Ctrl+Shift+J
    // (Scheduled jobs) were bound to nothing at all, and Ctrl+Shift+S was
    // printed beside two entries -- so somebody reading the menu and pressing it
    // to add a file to a set started a folder-size measurement of everything in
    // view. See MOLE-396.
    //
    // **The two lists are one list now**: the shell keeps no key text at all, and
    // each `Shortcut` hands its own `nativeText` to the thing it reaches through
    // `App.declareShortcut()`. So what this case holds has changed with it -- a
    // label cannot disagree with an accelerator that does not exist separately --
    // and what can still go wrong is the pairing: a Shortcut declaring one id and
    // triggering another, a declaration for an id nothing registers, two
    // Shortcuts claiming one entry, and the in-window key list, which is prose
    // and was wrong in the same release. See MOLE-416.
    const QString menu = m_shellSources.value(QStringLiteral("ui/AppController.cpp"));
    QVERIFY2(!menu.isEmpty(), "the menu's own source was not read");

    // Every id the shell registers, and every feature id the built-ins claim: a
    // declaration naming anything else labels nothing.
    QSet<QString> knownTargets;
    {
        // Every id the shell spells. Not only `action.id = …`: three Help entries
        // are built from a table, and an id that reaches the registry by any
        // route is still an id a key can be declared for.
        static const QRegularExpression anId(QStringLiteral(R"rx(QStringLiteral\("(mole\.[\w.]+)"\))rx"));
        auto ids = anId.globalMatch(menu);
        while (ids.hasNext())
            knownTargets.insert(ids.next().captured(1));

        static const QRegularExpression featureId(
            QStringLiteral(R"rx(QStringLiteral\("(mole\.[\w.]+)"\))rx"));
        // Wherever a built-in happens to declare its own id -- some in a
        // config, some as an id() override, in a dozen files.
        const QString features = readSourcesUnder(QStringLiteral("plugins/builtin"));
        QVERIFY2(!features.isEmpty(), "the built-in features' sources were not read");
        auto claimed = featureId.globalMatch(features);
        while (claimed.hasNext())
            knownTargets.insert(claimed.next().captured(1));
    }
    QVERIFY2(knownTargets.size() >= 20,
        qPrintable(QStringLiteral("only %1 ids were found to declare keys for; the parse has stopped "
                                  "working")
                       .arg(knownTargets.size())));

    // Each Shortcut block, with what it declares and what it does.
    struct Declaration
    {
        QString target;
        QString body;
    };
    QList<Declaration> declarations;
    {
        static const QRegularExpression block(
            QStringLiteral(R"(Shortcut \{(.*?)\n    \})"), QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression declares(
            QStringLiteral(R"rx(App\.declareShortcut\("([^"]+)", this\))rx"));
        for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
            auto blocks = block.globalMatch(it.value());
            while (blocks.hasNext()) {
                const QString body = blocks.next().captured(1);
                const QRegularExpressionMatch said = declares.match(body);
                if (said.hasMatch())
                    declarations.append({ said.captured(1), body });
            }
        }
    }
    QVERIFY2(declarations.size() >= 12,
        qPrintable(QStringLiteral("only %1 keys are declared for a menu entry; the parse has stopped "
                                  "working")
                       .arg(declarations.size())));

    // A declaration outside a Shortcut block would label an entry with a key
    // nothing binds, which is the fault this arrangement exists to make
    // impossible.
    int declarationsAnywhere = 0;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it)
        declarationsAnywhere += it.value().count(QStringLiteral("App.declareShortcut("));
    QCOMPARE(declarationsAnywhere, declarations.size());

    static const QRegularExpression triggers(QStringLiteral(R"rx(App\.triggerAction\("([^"]+)"\))rx"));
    QSet<QString> claimed;
    for (const Declaration& declaration : declarations) {
        QVERIFY2(knownTargets.contains(declaration.target),
            qPrintable(QStringLiteral("a key is declared for \"%1\", which no action and no feature "
                                      "answers to")
                           .arg(declaration.target)));
        QVERIFY2(!claimed.contains(declaration.target),
            qPrintable(
                QStringLiteral("two shortcuts both claim to be the key for %1").arg(declaration.target)));
        claimed.insert(declaration.target);

        // Where the block says what it runs, it has to be the same thing it
        // labels. This is MOLE-396's fault in the shape it could still take: one
        // key, printed beside an entry that does something else.
        const QRegularExpressionMatch runs = triggers.match(declaration.body);
        if (runs.hasMatch()) {
            QCOMPARE(runs.captured(1), declaration.target);
        }
    }

    // The in-window key list is deliberately not held here. Half of what it names
    // is bound in a pane's own key handler rather than as a window Shortcut --
    // F2, F7, Ctrl+A, the arrows -- so a rule that asked the window to declare
    // every key in it would have to be widened until it meant nothing. What is
    // held is the pairing that MOLE-396 was reported for, above.
}

void TestQmlConventions::noIconOnlyControlIsSmallerThanTheFloor()
{
    // **Twenty-four were raised and six were missed**, two of them half-raised --
    // `implicitHeight: App.minimumTarget` with `implicitWidth: 24` left behind,
    // which is the copy-paste-with-one-edit shape. The walkthrough holds two
    // named controls at run time; these six had no names to hold, so this reads
    // the source instead and covers every one there is.
    //
    // Static because it is a claim about what is written rather than about what a
    // window did: a control declared at 24 is 24 on every machine, and a rule
    // read from the source goes on being true for controls nobody has written
    // yet. See MOLE-398 and theIconOnlyControlsAreBigEnoughToHit.
    static const QRegularExpression fixedSize(QStringLiteral("implicit(Width|Height)\\s*:\\s*([0-9]+)\\b"));

    QStringList tooSmall;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        const QStringList lines = it.value().split(QLatin1Char('\n'));
        for (int line = 0; line < lines.size(); ++line) {
            const QRegularExpressionMatch match = fixedSize.match(lines.at(line));
            if (!match.hasMatch())
                continue;
            if (match.captured(2).toInt() >= 28)
                continue;

            // Only a control: a busy indicator, a tag and a coloured dot are not
            // things to hit, and a floor for them would be a rule about
            // decoration. The kind is the enclosing declaration, so this looks
            // back for the nearest one.
            QString kind;
            for (int back = line; back >= 0 && back > line - 12; --back) {
                static const QRegularExpression opensAType(QStringLiteral("^\\s*([A-Z][A-Za-z]*)\\s*\\{"));
                const QRegularExpressionMatch opener = opensAType.match(lines.at(back));
                if (opener.hasMatch()) {
                    kind = opener.captured(1);
                    break;
                }
            }
            static const QStringList controls { QStringLiteral("ToolButton"), QStringLiteral("Button"),
                QStringLiteral("ActionButton"), QStringLiteral("RoundButton"), QStringLiteral("CheckBox"),
                QStringLiteral("RadioButton"), QStringLiteral("Switch") };
            if (!controls.contains(kind))
                continue;

            tooSmall.append(
                QStringLiteral("%1:%2 a %3 of %4").arg(it.key()).arg(line + 1).arg(kind, match.captured(2)));
        }
    }

    QVERIFY2(tooSmall.isEmpty(),
        qPrintable(QStringLiteral("a control smaller than the 28px floor: %1")
                       .arg(tooSmall.join(QStringLiteral(", ")))));
}

void TestQmlConventions::everyDialogIsBuiltOnTheOneBase()
{
    // **Nineteen dialogs pasted the same nine lines** -- the panel ground, both
    // overlay veils, `modal`, `focus`, `anchors.centerIn`, a width, and six lines
    // of comment about the first three. Three CMake greps stand in for the
    // component that makes the omission impossible, and they exist because the
    // paste was forgotten three times. The placement drifted with it: six
    // dialogs centred on `parent` -- the tab body, which is not the middle of the
    // window once there is a sidebar, and is a third of the way up it with the
    // terminal panel open -- and four fixed a width with no clamp while five
    // clamped.
    //
    // ui/MoleDialog.qml holds all of it once. This is the rule that keeps the
    // twentieth from being written the old way; the CMake greps stay as the
    // backstop for the veil in particular. See MOLE-398.
    static const QRegularExpression rawDialog(QStringLiteral("(?m)^\\s*Dialog\\s*\\{"));
    QStringList raw;
    QStringList unclamped;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        if (it.key().endsWith(QStringLiteral("MoleDialog.qml")))
            continue;
        if (it.value().contains(rawDialog))
            raw.append(it.key());

        // A dialog centred on its parent lands in the middle of whatever
        // declared it. MoleDialog centres on the window's overlay; nothing else
        // may make that decision for a dialog -- so this looks *inside* each
        // MoleDialog block, at its own level. An `anchors.centerIn: parent` on a
        // label inside one is ordinary and is not this.
        const QStringList lines = it.value().split(QLatin1Char('\n'));
        for (int line = 0; line < lines.size(); ++line) {
            static const QRegularExpression opensADialog(
                QStringLiteral("^(\\s*)(MoleDialog|SingleFieldDialog)\\s*\\{"));
            const QRegularExpressionMatch opener = opensADialog.match(lines.at(line));
            if (!opener.hasMatch())
                continue;
            const QString inside = opener.captured(1) + QStringLiteral("    ");
            for (int at = line + 1; at < lines.size(); ++at) {
                // Its own level only, and the block ends at the closing brace on
                // the opener's indentation.
                if (lines.at(at) == opener.captured(1) + QLatin1Char('}'))
                    break;
                if (lines.at(at) == inside + QStringLiteral("anchors.centerIn: parent"))
                    unclamped.append(QStringLiteral("%1:%2").arg(it.key()).arg(at + 1));
            }
        }
    }

    QVERIFY2(raw.isEmpty(),
        qPrintable(QStringLiteral("a dialog is declared without the base that carries the veil, "
                                  "the ground, the focus and the clamp: %1")
                       .arg(raw.join(QStringLiteral(", ")))));
    QVERIFY2(unclamped.isEmpty(),
        qPrintable(QStringLiteral("a dialog centres on its parent rather than on the window: %1")
                       .arg(unclamped.join(QStringLiteral(", ")))));

    // And the base itself says all nine things, or removing them from nineteen
    // files removed them altogether.
    const QString base = m_sources.value(QStringLiteral("MoleDialog.qml"));
    QVERIFY2(!base.isEmpty(), "ui/MoleDialog.qml was not read");
    for (const QString& line : { QStringLiteral("Material.background: App.colour.panel"),
             QStringLiteral("Overlay.modal: DimVeil {}"), QStringLiteral("Overlay.modeless: DimVeil {}"),
             QStringLiteral("modal: true"), QStringLiteral("focus: true"),
             QStringLiteral("anchors.centerIn: Overlay.overlay"),
             QStringLiteral("Math.min(preferredWidth") }) {
        QVERIFY2(
            base.contains(line), qPrintable(QStringLiteral("ui/MoleDialog.qml does not say: %1").arg(line)));
    }
}

void TestQmlConventions::theMenuHeadingsAreTheOnesTheDocumentsAndTheTooltipsName()
{
    // **The menu has six headings and four documents said four.** README.md and
    // ARCHITECTURE.md listed "File / View / Tools / Help" long after Tools was
    // split into Operations and Workflows, the searching guide told the reader to
    // look under "Tools ▸", and five tooltips *in the window* pointed at that
    // heading too -- so the application itself sent people to a menu that has not
    // existed for months. Nothing compared the two, which is how it lasted. See
    // MOLE-402 and MOLE-392.
    const QString registry = readSource(QStringLiteral("host/ActionRegistry.cpp"));
    QVERIFY2(!registry.isEmpty(), "the action registry's own source was not read");

    // The headings, in the order sectionTitle() answers them, which is the order
    // the menu is built in.
    static const QRegularExpression titles(
        QStringLiteral(R"rx(case MenuAction::Section::\w+:\s*\n\s*return QStringLiteral\("([^"]+)"\))rx"));
    QStringList headings;
    auto found = titles.globalMatch(registry);
    while (found.hasNext())
        headings.append(found.next().captured(1));
    QVERIFY2(headings.size() >= 5,
        qPrintable(QStringLiteral("found %1 menu headings, which is not the menu").arg(headings.size())));

    // Every document that names the set has to name all of it. Read as one line
    // each, because both write it as a table cell.
    for (const QString& document : { QStringLiteral("README.md"), QStringLiteral("ARCHITECTURE.md") }) {
        const QString text = readDocument(document);
        QVERIFY2(!text.isEmpty(), qPrintable(document));
        static const QRegularExpression names(QStringLiteral(R"(entry under ([^|\n]+))"));
        const QRegularExpressionMatch match = names.match(text);
        QVERIFY2(match.hasMatch(),
            qPrintable(QStringLiteral("%1 does not say what the menu headings are").arg(document)));
        const QString said = match.captured(1);
        for (const QString& heading : headings) {
            QVERIFY2(said.contains(heading),
                qPrintable(QStringLiteral("%1 lists the menu headings as \"%2\" and leaves out %3")
                               .arg(document, said.trimmed(), heading)));
        }
    }

    // And nothing in the window points at a heading that is not one of them. The
    // form is the one the tooltips use: "Operations ▸ Index this folder".
    static const QRegularExpression pointsAt(QStringLiteral(R"(([A-Z][A-Za-z]+) \x{25b8})"));
    for (auto it = m_shellSources.constBegin(); it != m_shellSources.constEnd(); ++it) {
        auto mentions = pointsAt.globalMatch(it.value());
        while (mentions.hasNext()) {
            const QString named = mentions.next().captured(1);
            QVERIFY2(headings.contains(named),
                qPrintable(QStringLiteral("%1 sends the reader to \"%2 \u25b8\", and there is no such "
                                          "menu heading")
                               .arg(it.key(), named)));
        }
    }
}

void TestQmlConventions::noImageDecodeIsBoundToALiveSize()
{
    // **Changing `sourceSize` makes Image reload.** ImagePreview bound it to the
    // live pane width, so dragging the sidebar or the details divider issued a
    // decode per pixel of width -- the picture flickering through Image.Loading
    // for the length of the drag, on every intermediate size nobody was looking
    // at. Quantised to the next 256 px, a drag across a pane costs a handful of
    // decodes instead of hundreds.
    //
    // Read from the source rather than counted at run time: what is being
    // asserted is that the request does not change with every pixel, and a
    // binding either quantises or it does not. A count of `Image.status`
    // transitions across a resize would measure the same thing through a window
    // manager. See MOLE-398.
    static const QRegularExpression sourceSize(
        QStringLiteral("(?m)^\\s*sourceSize\\.(width|height)\\s*:([^\\n]*)$"));

    QStringList live;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        auto found = sourceSize.globalMatch(it.value());
        while (found.hasNext()) {
            const QRegularExpressionMatch match = found.next();
            const QString binding = match.captured(2);
            // A constant, or nothing at all, is fine. What is not is a width or a
            // height straight off an item, with nothing rounding it.
            const bool reads
                = binding.contains(QStringLiteral(".width")) || binding.contains(QStringLiteral(".height"));
            const bool quantised = binding.contains(QStringLiteral("Math.ceil"))
                || binding.contains(QStringLiteral("Math.floor"))
                || binding.contains(QStringLiteral("Math.round"));
            if (reads && !quantised)
                live.append(QStringLiteral("%1: %2").arg(it.key(), binding.trimmed()));
        }
    }

    QVERIFY2(live.isEmpty(),
        qPrintable(QStringLiteral("an image decode is bound to a live size, so a drag re-decodes "
                                  "on every pixel: %1")
                       .arg(live.join(QStringLiteral(", ")))));
}

void TestQmlConventions::nothingPaintsWithTheStyleOrADerivation()
{
    // **`nothingNamesAColourByHand` greps for "#rrggbb", so every other way of
    // bypassing the token layer passed it** -- and several did.
    //
    // `Material.foreground` painted the most-read text in the application: the
    // file name in both FilePane delegates and the sidebar's labels. It is the
    // Material style's computed text colour for the polarity, not
    // `App.colour.text`, so a theme that set `text` to anything but Material's
    // default had no effect on the listing at all. `Material.accent` was used as
    // a colour value in twenty-four places across twelve files, tracking the
    // theme only because Main.qml binds it -- and those same files named
    // `App.colour.accent` elsewhere, so one file named one token two ways.
    //
    // Then the derivations ADR-0072 does not allow: `Qt.lighter(window, 1.1)` for
    // a split handle, `Qt.darker(accent, 1.3)` for a pressed button, and
    // `Qt.hsla((index * 0.13) % 1.0, 0.45, 0.58, 1.0)` for a chart -- a fixed
    // lightness never looked at on a light ground. And an opacity standing in for
    // a token: an accent at 0.18 where `selection` exists, a pane at 0.85 for an
    // inactive frame, two hex marks at 0.28, and disabled words at 0.4.
    //
    // See MOLE-397, ADR-0072 and its amendment. What is still allowed is listed
    // below, each with its reason.
    QStringList offenders;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        const QStringList lines = it.value().split(QLatin1Char('\n'));
        for (int i = 0; i < lines.size(); ++i) {
            const QString line = lines.at(i);
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QStringLiteral("//")))
                continue;
            const auto complain = [&](const QString& what) {
                offenders.append(QStringLiteral("%1:%2 %3").arg(it.key()).arg(i + 1).arg(what));
            };

            // Reading the style's colours. Setting them is the opposite: Main.qml
            // hands Material its accent *from* a token, which is what makes Qt's
            // own controls follow the theme, and a Material.background: is a
            // ground named the way ADR-0074 asks.
            static const QRegularExpression readsTheStyle(
                QStringLiteral("(?<!Material\\.)\\bMaterial\\.(accent|foreground|primary)\\b"));
            if (line.contains(readsTheStyle) && !trimmed.startsWith(QStringLiteral("Material.")))
                complain(QStringLiteral("paints with Material.accent/foreground/primary"));

            // A generated colour has no polarity and nobody has looked at it.
            if (line.contains(QStringLiteral("Qt.hsla(")) || line.contains(QStringLiteral("Qt.hsva(")))
                complain(QStringLiteral("generates a colour with Qt.hsla"));

            // Deriving in the view. `Qt.rgba` is left alone: DimVeil and the
            // terminal use it for a veil over whatever is behind them, which is
            // not one of the window's colours written down twice.
            if (line.contains(QStringLiteral("Qt.lighter(")) || line.contains(QStringLiteral("Qt.darker(")))
                complain(QStringLiteral("derives a colour in the view"));

            // An opacity on something that paints a colour is a token nobody
            // named: the result depends on whatever happens to be underneath.
            static const QRegularExpression fadedColour(
                QStringLiteral("^\\s*opacity:\\s*(0\\.[0-9]+|[^\\n]*\\?[^\\n]*0\\.[0-9]+)"));
            if (line.contains(fadedColour)) {
                // Only when the same block paints a colour. A fade of an icon, a
                // grip appearing on hover or a whole panel sliding in is motion,
                // not a colour choice.
                bool paints = false;
                bool ownPalette = false;
                for (int at = i; at >= 0 && at > i - 8; --at) {
                    if (lines.at(at).contains(QStringLiteral("color:")))
                        paints = true;
                    // A colour from the *screen's* own sixteen is not one of the
                    // window's tokens, and the terminal cursor is translucent so
                    // that the character underneath can still be read -- which is
                    // a requirement rather than a colour nobody named.
                    if (lines.at(at).contains(QStringLiteral("colourFor(")))
                        ownPalette = true;
                    if (lines.at(at).trimmed().endsWith(QLatin1Char('{')))
                        break;
                }
                if (paints && !ownPalette)
                    complain(QStringLiteral("fades a painted colour instead of naming a token"));
            }
        }
    }

    std::sort(offenders.begin(), offenders.end());
    QVERIFY2(offenders.isEmpty(),
        qPrintable(QStringLiteral("the token layer is bypassed at:\n    %1")
                       .arg(offenders.join(QStringLiteral("\n    ")))));
}

MOLE_TEST_MAIN_GUI(TestQmlConventions)
#include "tst_QmlConventions.moc"
