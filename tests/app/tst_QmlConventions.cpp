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

void TestQmlConventions::everyKeyTheMenuAdvertisesIsAKeyTheWindowDeclares()
{
    // **`MenuAction::shortcut` is display-only**, and Main.qml's own comment says
    // a key named in the menu and not declared there "would be advertised and do
    // nothing". Five were in exactly that state: Ctrl+Shift+A (Analyse),
    // Ctrl+Shift+R (Bulk rename), Ctrl+Shift+L (Alerts) and Ctrl+Shift+J
    // (Scheduled jobs) were bound to nothing at all, and Ctrl+Shift+S was
    // printed beside two entries -- so somebody reading the menu and pressing it
    // to add a file to a set started a folder-size measurement of everything in
    // view. The command palette repeated the same wrong hint.
    //
    // Nothing compared the two lists, which is how five accumulated. Read from
    // the source because that is where both halves live: a label in
    // AppController::buildActions() and a `sequence:` in the window's QML. No
    // window is needed to ask whether they agree. See MOLE-396.
    const QString menu = m_shellSources.value(QStringLiteral("ui/AppController.cpp"));
    QVERIFY2(!menu.isEmpty(), "the menu's own source was not read");

    // Cut into one chunk per action first, and only then look for a label inside
    // it: an expression that reaches from an id to "the next shortcut" pairs an
    // action that has no key with a label belonging to a later one, which is a
    // report naming the wrong entry.
    static const QRegularExpression declaresAnId(
        QStringLiteral("action\\.id = QStringLiteral\\(\"([^\"]+)\"\\)"));
    static const QRegularExpression declaresAShortcut(
        QStringLiteral("action\\.shortcut = QStringLiteral\\(\"([^\"]+)\"\\)"));

    QList<QPair<QString, QString>> labelledActions; // id -> key, one per action
    {
        QList<QPair<int, QString>> starts;
        auto ids = declaresAnId.globalMatch(menu);
        while (ids.hasNext()) {
            const QRegularExpressionMatch match = ids.next();
            starts.append({ static_cast<int>(match.capturedStart()), match.captured(1) });
        }
        for (int i = 0; i < starts.size(); ++i) {
            const int from = starts.at(i).first;
            const int to = i + 1 < starts.size() ? starts.at(i + 1).first : menu.size();
            const QRegularExpressionMatch key = declaresAShortcut.match(menu.mid(from, to - from));
            if (key.hasMatch())
                labelledActions.append({ starts.at(i).second, key.captured(1) });
        }
    }

    // What the window declares, read from the `sequence:` and `sequences:`
    // properties themselves -- not from anywhere a key is merely mentioned. The
    // in-window key dialog lists the same strings as prose, so a check that
    // looked for the text anywhere in the file called every advertised key
    // declared, including the four that did nothing.
    QSet<QString> declaredKeys;
    {
        static const QRegularExpression property(
            QStringLiteral("sequences?\\s*:\\s*(\\[[^\\]]*\\]|\"[^\"]*\")"));
        static const QRegularExpression quoted(QStringLiteral("\"([^\"]+)\""));
        static const QRegularExpression standardKey(QStringLiteral("StandardKey\\.([A-Za-z]+)"));
        const QMetaEnum standard = QMetaEnum::fromType<QKeySequence::StandardKey>();
        QVERIFY2(standard.isValid(), "QKeySequence::StandardKey is not a readable enum here");

        for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
            auto properties = property.globalMatch(it.value());
            while (properties.hasNext()) {
                const QString value = properties.next().captured(1);

                auto strings = quoted.globalMatch(value);
                while (strings.hasNext())
                    declaredKeys.insert(strings.next().captured(1));

                // A StandardKey is resolved through Qt rather than through the
                // comment beside it: Ctrl+Q is `StandardKey.Quit` here, and what
                // that means is Qt's answer and this platform's.
                auto named = standardKey.globalMatch(value);
                while (named.hasNext()) {
                    bool known = false;
                    const int enumerator
                        = standard.keyToValue(named.next().captured(1).toUtf8().constData(), &known);
                    if (!known)
                        continue;
                    const QList<QKeySequence> bound
                        = QKeySequence::keyBindings(static_cast<QKeySequence::StandardKey>(enumerator));
                    for (const QKeySequence& sequence : bound)
                        declaredKeys.insert(sequence.toString(QKeySequence::PortableText));
                }
            }
        }
    }
    QVERIFY2(declaredKeys.size() >= 10,
        qPrintable(QStringLiteral("only %1 keys are declared anywhere in the window; the parse has "
                                  "stopped working")
                       .arg(declaredKeys.size())));

    QHash<QString, QString> advertisedBy; // key sequence -> action id
    QStringList clashes;
    QStringList undeclared;

    // A key sequence rather than a sentence: "type to find, or /" and "type to
    // filter" are prose in the same field, and are not claims about a binding.
    static const QRegularExpression looksLikeAKey(QStringLiteral("^(Ctrl|Alt|Shift|Meta)\\+|^F[0-9]{1,2}$"));

    int labels = 0;
    for (const auto& [id, key] : std::as_const(labelledActions)) {
        if (!looksLikeAKey.match(key).hasMatch())
            continue;
        ++labels;

        if (advertisedBy.contains(key) && advertisedBy.value(key) != id) {
            clashes.append(
                QStringLiteral("%1 is printed beside both %2 and %3").arg(key, advertisedBy.value(key), id));
        } else {
            advertisedBy.insert(key, id);
        }

        if (!declaredKeys.contains(key))
            undeclared.append(QStringLiteral("%1 (%2)").arg(key, id));
    }

    // The guard: a parse that found nothing would pass this without reading a
    // thing, and the two expressions above are exactly the kind that stop
    // matching when somebody reformats the file.
    QVERIFY2(labels >= 8,
        qPrintable(QStringLiteral("only %1 shortcut labels were found in the menu; the parse has "
                                  "stopped working")
                       .arg(labels)));

    QVERIFY2(clashes.isEmpty(), qPrintable(clashes.join(QStringLiteral("; "))));
    QVERIFY2(undeclared.isEmpty(),
        qPrintable(QStringLiteral("the menu advertises keys the window does not declare: %1")
                       .arg(undeclared.join(QStringLiteral(", ")))));
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
