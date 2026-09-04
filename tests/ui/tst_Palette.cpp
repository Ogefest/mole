#include "plugins/builtin/previews/SyntaxHighlighter.h"
#include "support/MoleTestMain.h"
#include "ui/AppController.h"
#include "ui/Palette.h"

#include <QMetaProperty>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>

#include <array>
#include <cmath>
#include <utility>

using namespace mole;

/// The palette, the themes in it, and the one mistake that leaves no other trace.
class TestPalette : public QObject
{
    Q_OBJECT

private slots:
    void aBindingFollowsTheTokenRatherThanItsStartupValue();
    void noTokenIsConstant();
    void aThemeMovesEveryTokenAndSaysSoOnce();
    void askingForTheThemeAlreadyInForceSaysNothing();
    void aThemeNobodyShipsOpensOnTheDefault();
    void midnightIsWhatMoleAlreadyLookedLike();
    void everyShippedThemeIsAWholePalette();
    void everyThemeMeetsAStatedContrastFloor();
    void everyDerivedTokenIsVisibleOnBothPolarities();
};

namespace {

/// The sixteen names a view is allowed to say, each paired with the field behind
/// it, so the QML name and the value are asserted against one list rather than
/// two that can drift apart.
struct Token
{
    const char* name;
    QColor Palette::Tokens::*field;
};

const std::array<Token, 16> kTokens { {
    { "window", &Palette::Tokens::window },
    { "panel", &Palette::Tokens::panel },
    { "pane", &Palette::Tokens::pane },
    { "border", &Palette::Tokens::border },
    { "hover", &Palette::Tokens::hover },
    { "selection", &Palette::Tokens::selection },
    { "text", &Palette::Tokens::text },
    { "textSecondary", &Palette::Tokens::textSecondary },
    { "textMuted", &Palette::Tokens::textMuted },
    { "textFaint", &Palette::Tokens::textFaint },
    { "accent", &Palette::Tokens::accent },
    { "link", &Palette::Tokens::link },
    { "ok", &Palette::Tokens::ok },
    { "warn", &Palette::Tokens::warn },
    { "bad", &Palette::Tokens::bad },
    { "busy", &Palette::Tokens::busy },
} };

/// WCAG 2.1's relative luminance and contrast ratio, which is the only widely
/// agreed answer to "can this be read". Eight lines rather than a dependency.
double luminance(const QColor& c)
{
    const auto channel
        = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
}

double contrast(const QColor& a, const QColor& b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

} // namespace

/// The trap the whole scheme turns on, and the only test that can catch it.
///
/// Declare the token properties `CONSTANT` and QML evaluates each binding once:
/// the window keeps whatever the palette held when it loaded, choosing a theme
/// afterwards does nothing at all, and Qt says nothing about it -- no warning,
/// nothing in the log, every value in the file correct. So this binds through the
/// path a view actually uses, `App.colour.panel`, changes the theme underneath it,
/// and requires the binding to have moved.
void TestPalette::aBindingFollowsTheTokenRatherThanItsStartupValue()
{
    AppController app;
    QVERIFY(app.colour() != nullptr);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &app);

    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral("import QtQuick\n"
                                        "QtObject { property color seen: App.colour.panel }\n"),
        QUrl(QStringLiteral("qrc:/tst_Palette/binding.qml")));
    const std::unique_ptr<QObject> bound(component.create());
    QVERIFY2(bound != nullptr, qPrintable(component.errorString()));

    QCOMPARE(bound->property("seen").value<QColor>(), Palette::midnight().panel);

    QVERIFY(app.colour()->setTheme(QStringLiteral("Slate")));
    QCOMPARE(bound->property("seen").value<QColor>(), Palette::slate().panel);
}

/// The same claim stated against the meta-object, which is where it can be
/// checked one token at a time. The binding test above catches the mistake; this
/// one says which of the sixteen made it.
void TestPalette::noTokenIsConstant()
{
    const Palette palette;
    const QMetaObject* meta = palette.metaObject();
    for (const Token& token : kTokens) {
        const int index = meta->indexOfProperty(token.name);
        QVERIFY2(index >= 0,
            qPrintable(QStringLiteral("no property called %1 -- a view naming it reads undefined")
                           .arg(QLatin1String(token.name))));
        QVERIFY2(meta->property(index).hasNotifySignal(),
            qPrintable(QStringLiteral("%1 is CONSTANT, so every binding on it is frozen at startup")
                           .arg(QLatin1String(token.name))));
    }
}

void TestPalette::aThemeMovesEveryTokenAndSaysSoOnce()
{
    Palette palette;
    QCOMPARE(palette.theme(), Palette::defaultTheme());

    QSignalSpy changed(&palette, &Palette::changed);
    QVERIFY(palette.setTheme(QStringLiteral("Slate")));
    QCOMPARE(palette.theme(), QStringLiteral("Slate"));

    // One signal for the sixteen, because they only ever move together. Sixteen
    // would be fifteen further passes over every binding in the window.
    QCOMPARE(changed.count(), 1);

    // Named one at a time: "a token did not move" is not a lead. And a theme that
    // repeated one of Midnight's values would leave that part of the window as it
    // was, with nothing to say so.
    const Palette::Tokens slate = Palette::slate();
    const Palette::Tokens midnight = Palette::midnight();
    for (const Token& token : kTokens) {
        const QVariant read = palette.property(token.name);
        QVERIFY2(read.isValid(), token.name);
        QVERIFY2(read.value<QColor>() == slate.*(token.field),
            qPrintable(QStringLiteral("%1 is %2, expected %3")
                           .arg(QLatin1String(token.name), read.value<QColor>().name(),
                               (slate.*(token.field)).name())));
        QVERIFY2((slate.*(token.field)) != (midnight.*(token.field)),
            qPrintable(QStringLiteral("Slate's %1 is Midnight's, so choosing it leaves that alone")
                           .arg(QLatin1String(token.name))));
    }
}

void TestPalette::askingForTheThemeAlreadyInForceSaysNothing()
{
    Palette palette;
    QSignalSpy changed(&palette, &Palette::changed);
    QVERIFY(palette.setTheme(Palette::defaultTheme()));
    QCOMPARE(changed.count(), 0);
}

/// A preferences file from a newer build, or a name somebody typed by hand.
void TestPalette::aThemeNobodyShipsOpensOnTheDefault()
{
    Palette palette;
    QVERIFY(palette.setTheme(QStringLiteral("Slate")));

    QTest::ignoreMessage(QtWarningMsg, "No theme called Sunset; opening on Midnight");
    QVERIFY2(!palette.setTheme(QStringLiteral("Sunset")), "an unknown name has to be reported");
    QCOMPARE(palette.theme(), Palette::defaultTheme());
    QCOMPARE(palette.panel(), Palette::midnight().panel);
}

/// What Mole looks like, written down a second time, so that changing it has to
/// be a deliberate edit in two places rather than a slip in one.
void TestPalette::midnightIsWhatMoleAlreadyLookedLike()
{
    const Palette::Tokens m = Palette::midnight();
    QCOMPARE(m.window.name(), QStringLiteral("#151922"));
    QCOMPARE(m.panel.name(), QStringLiteral("#1b2029"));
    QCOMPARE(m.pane.name(), QStringLiteral("#151922"));
    QCOMPARE(m.border.name(), QStringLiteral("#2a3140"));
    QCOMPARE(m.hover.name(), QStringLiteral("#232a36"));
    QCOMPARE(m.selection.name(), QStringLiteral("#26303f"));
    QCOMPARE(m.text.name(), QStringLiteral("#e6ebf5"));
    QCOMPARE(m.textSecondary.name(), QStringLiteral("#c9d1e0"));
    QCOMPARE(m.textMuted.name(), QStringLiteral("#8b93a7"));
    QCOMPARE(m.textFaint.name(), QStringLiteral("#6f7788"));
    QCOMPARE(m.accent.name(), QStringLiteral("#4c9aff"));
    QCOMPARE(m.link.name(), QStringLiteral("#7cc4ff"));
    QCOMPARE(m.ok.name(), QStringLiteral("#57ab5a"));
    QCOMPARE(m.warn.name(), QStringLiteral("#d9a441"));
    QCOMPARE(m.bad.name(), QStringLiteral("#e5534b"));
    QCOMPARE(m.busy.name(), QStringLiteral("#1e2a3a"));
}

/// The shipped list, and the one property every theme has to have: sixteen
/// colours, none of them left unset. A `Tokens` is a plain struct, so a theme
/// that forgot a field would hand the window an invalid colour rather than fail
/// to build.
void TestPalette::everyShippedThemeIsAWholePalette()
{
    const QStringList names = Palette::themeNames();
    QCOMPARE(names,
        QStringList({ QStringLiteral("Midnight"), QStringLiteral("Slate"), QStringLiteral("Paper"),
            QStringLiteral("Workbench") }));
    QCOMPARE(Palette::defaultTheme(), QStringLiteral("Midnight"));

    int lightThemes = 0;
    for (const QString& name : names) {
        const Palette::Tokens t = Palette::tokensFor(name);
        for (const Token& token : kTokens) {
            QVERIFY2((t.*(token.field)).isValid(),
                qPrintable(QStringLiteral("%1's %2 is not a colour").arg(name, QLatin1String(token.name))));
        }
        // The polarity is stated per theme, so what is worth checking is that the
        // values agree with what was stated: text on the far side of the ground
        // from it, either way up. A theme that said light and painted dark would
        // take `Material.theme` and both document colour sets with it.
        const bool light = Palette::isLightTheme(name);
        lightThemes += light ? 1 : 0;
        QVERIFY2(light == (t.text.lightness() < t.pane.lightness()),
            qPrintable(QStringLiteral("%1 says %2 and paints the other way")
                           .arg(name, light ? QStringLiteral("light") : QStringLiteral("dark"))));
    }

    // Two of each, because a chooser with three dark entries and one light one is
    // not what was decided.
    QCOMPARE(lightThemes, 2);
}

/// The test the source highlighter's nine colours would have failed for years.
///
/// `kStringColour` was `#a5d6a7`, a pastel green picked against `#151922`. On white
/// it is about 1.7:1, at which point a string literal stops being text -- and
/// nothing in the tree would have said so, because a colour that is wrong is still
/// a colour. So this is a table of the pairs that actually meet on screen, with a
/// floor per pair and the reason for the floor beside it, run over every theme.
/// Anything added after these four is held to the same numbers.
void TestPalette::everyDerivedTokenIsVisibleOnBothPolarities()
{
    // **The derivations used to live in the views, and nobody had looked at them
    // on both grounds.** `Qt.lighter(App.colour.window, 1.1)` for a split handle
    // is a step towards white, which on a light theme is a step towards
    // invisible; `Qt.darker(App.colour.accent, 1.3)` for a pressed button walks
    // towards black on a dark theme, where it should walk away from it; and a
    // chart's `Qt.hsla(..., 0.45, 0.58, 1.0)` has one fixed lightness for every
    // theme there is. They are computed in the palette now, from the sixteen, and
    // this is the case that says they can be seen. See MOLE-397.
    QStringList failures;
    for (const QString& name : Palette::themeNames()) {
        Palette palette;
        QVERIFY(palette.setTheme(name));
        const Palette::Tokens t = Palette::tokensFor(name);

        // A grip has to be distinguishable from the ground it sits in. 1.15 is
        // the floor a hairline is held to, and this is the same kind of mark.
        const double divider = contrast(palette.divider(), t.window);
        if (divider + 0.005 < 1.15)
            failures.append(QStringLiteral("%1: divider on window is %2:1").arg(name).arg(divider));

        // A pressed button is still a button: it has to be visible against the
        // window the way the accent is, and it has to *look* pressed -- a press
        // that leaves the colour where it was is a button that does not answer.
        // Which pair carries the label is ActionButton's business and not this
        // token's, which is why the floor here is the graphic one.
        const double pressedOnWindow = contrast(palette.accentPressed(), t.window);
        if (pressedOnWindow + 0.005 < 3.0) {
            failures.append(
                QStringLiteral("%1: accentPressed on window is %2:1").arg(name).arg(pressedOnWindow));
        }
        if (palette.accentPressed() == t.accent)
            failures.append(QStringLiteral("%1: accentPressed is the accent itself").arg(name));

        // A mark laid over text has to leave the text readable, which is the
        // whole reason it is not the accent itself.
        const double marked = contrast(t.text, palette.mark());
        if (marked + 0.005 < 3.0)
            failures.append(QStringLiteral("%1: text on mark is %2:1").arg(name).arg(marked));

        // Every category has to be visible on the ground a chart is drawn on, and
        // distinguishable from the one before it.
        const QVariantList categorical = palette.categorical();
        if (categorical.size() < 4)
            failures.append(QStringLiteral("%1: only %2 chart colours").arg(name).arg(categorical.size()));
        for (int i = 0; i < categorical.size(); ++i) {
            const QColor colour = categorical.at(i).value<QColor>();
            const double onPanel = contrast(colour, t.panel);
            if (onPanel + 0.005 < 1.6) {
                failures.append(
                    QStringLiteral("%1: chart colour %2 on panel is %3:1").arg(name).arg(i).arg(onPanel));
            }
            if (i > 0 && colour == categorical.at(i - 1).value<QColor>())
                failures.append(
                    QStringLiteral("%1: chart colours %2 and %3 are the same").arg(name).arg(i - 1).arg(i));
        }
    }

    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("\n"))));
}

void TestPalette::everyThemeMeetsAStatedContrastFloor()
{
    struct Pair
    {
        const char* foreground;
        QColor Palette::Tokens::*fg;
        const char* background;
        QColor Palette::Tokens::*bg;
        double floor;
        const char* why;
    };

    // 4.5 is WCAG AA for text at this size. 3.0 is AA for large text and for a
    // graphic that only has to be *seen*. Where a floor is lower than 4.5 the
    // reason is in the row.
    static const Pair kPairs[] = {
        { "text", &Palette::Tokens::text, "pane", &Palette::Tokens::pane, 4.5, "a file name" },
        { "text", &Palette::Tokens::text, "panel", &Palette::Tokens::panel, 4.5, "a dialog's prose" },
        { "text", &Palette::Tokens::text, "hover", &Palette::Tokens::hover, 4.5, "a row under the pointer" },
        { "text", &Palette::Tokens::text, "selection", &Palette::Tokens::selection, 4.5,
            "the row Enter would act on" },
        { "textSecondary", &Palette::Tokens::textSecondary, "pane", &Palette::Tokens::pane, 4.5,
            "a size, a date" },
        { "textSecondary", &Palette::Tokens::textSecondary, "panel", &Palette::Tokens::panel, 4.5,
            "a column header" },
        { "textMuted", &Palette::Tokens::textMuted, "panel", &Palette::Tokens::panel, 4.5,
            "a label, a count -- quiet, still meant to be read" },
        { "textMuted", &Palette::Tokens::textMuted, "pane", &Palette::Tokens::pane, 4.5, "a keyboard hint" },
        { "textMuted", &Palette::Tokens::textMuted, "window", &Palette::Tokens::window, 4.5,
            "the toolbar's hints" },
        { "textMuted", &Palette::Tokens::textMuted, "busy", &Palette::Tokens::busy, 4.5,
            "the task strip while it is working" },
        // The floor, and the only place in the palette that is allowed to be hard
        // to read: a placeholder and a disabled control. WCAG exempts both. The
        // floor is here so it cannot disappear altogether.
        { "textFaint", &Palette::Tokens::textFaint, "panel", &Palette::Tokens::panel, 2.5, "a placeholder" },
        { "textFaint", &Palette::Tokens::textFaint, "pane", &Palette::Tokens::pane, 2.5, "a disabled row" },
        // Graphic rather than text: a focus ring, the active pane's border, a
        // four-pixel capacity bar.
        { "accent", &Palette::Tokens::accent, "window", &Palette::Tokens::window, 3.0, "a focus ring" },
        { "accent", &Palette::Tokens::accent, "panel", &Palette::Tokens::panel, 3.0, "the active tab" },
        { "accent", &Palette::Tokens::accent, "pane", &Palette::Tokens::pane, 3.0,
            "the active pane's border" },
        { "link", &Palette::Tokens::link, "panel", &Palette::Tokens::panel, 3.0, "a badge" },
        { "link", &Palette::Tokens::link, "pane", &Palette::Tokens::pane, 3.0, "something to follow" },
        // A semantic colour carries words as well as marks, so it is nearer text
        // than graphic -- but never alone. ADR-0010: the letter or the word is the
        // signal and the colour agrees with it. Hence 4.0 rather than 4.5.
        { "ok", &Palette::Tokens::ok, "panel", &Palette::Tokens::panel, 4.0, "a drive that is fine" },
        { "warn", &Palette::Tokens::warn, "panel", &Palette::Tokens::panel, 4.0, "a drive nearly full" },
        { "bad", &Palette::Tokens::bad, "panel", &Palette::Tokens::panel, 4.0, "a failure" },
        { "ok", &Palette::Tokens::ok, "pane", &Palette::Tokens::pane, 4.0, "an added file" },
        { "warn", &Palette::Tokens::warn, "pane", &Palette::Tokens::pane, 4.0, "a modified file" },
        { "bad", &Palette::Tokens::bad, "pane", &Palette::Tokens::pane, 4.0, "a conflicted file" },
        // A hairline only has to be seen at all. Below this it is not there.
        { "border", &Palette::Tokens::border, "panel", &Palette::Tokens::panel, 1.15, "a hairline" },
        { "border", &Palette::Tokens::border, "pane", &Palette::Tokens::pane, 1.15, "a pane's edge" },
    };

    // A source file's colours sit on whatever the text area is painted in, which
    // is the window's ground. The comment is the one that is quiet on purpose --
    // a comment is the thing in a source file you are meant to be able to skip.
    static const char* kCodeNames[]
        = { "key", "string", "number", "keyword", "builtin", "tag", "attribute", "comment", "preprocessor" };
    constexpr int kCommentIndex = 7;

    QStringList failures;
    for (const QString& name : Palette::themeNames()) {
        const Palette::Tokens t = Palette::tokensFor(name);
        for (const Pair& pair : kPairs) {
            const double ratio = contrast(t.*(pair.fg), t.*(pair.bg));
            if (ratio + 0.005 >= pair.floor)
                continue;
            failures.append(QStringLiteral("%1: %2 on %3 is %4:1, floor %5 (%6)")
                                .arg(name, QLatin1String(pair.foreground), QLatin1String(pair.background),
                                    QString::number(ratio, 'f', 2), QString::number(pair.floor, 'f', 2),
                                    QLatin1String(pair.why)));
        }

        const QStringList code = SourceHighlighter::coloursFor(Palette::isLightTheme(name));
        QCOMPARE(code.size(), 9);
        for (int i = 0; i < code.size(); ++i) {
            const double floor = i == kCommentIndex ? 3.0 : 4.5;
            for (const auto& ground :
                { std::make_pair("window", t.window), std::make_pair("pane", t.pane) }) {
                const double ratio = contrast(QColor(code.at(i)), ground.second);
                if (ratio + 0.005 >= floor)
                    continue;
                failures.append(
                    QStringLiteral("%1: source %2 (%3) on %4 is %5:1, floor %6")
                        .arg(name, QLatin1String(kCodeNames[i]), code.at(i), QLatin1String(ground.first),
                            QString::number(ratio, 'f', 2), QString::number(floor, 'f', 1)));
            }
        }
    }

    QVERIFY2(failures.isEmpty(), qPrintable(QLatin1Char('\n') + failures.join(QLatin1Char('\n'))));
}

MOLE_TEST_MAIN(TestPalette)
#include "tst_Palette.moc"
