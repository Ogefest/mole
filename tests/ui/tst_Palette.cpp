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

using namespace mole;

/// The palette, and the one mistake in it that leaves no other trace.
class TestPalette : public QObject
{
    Q_OBJECT

private slots:
    void aBindingFollowsTheTokenRatherThanItsStartupValue();
    void noTokenIsConstant();
    void everyTokenMovesAndTheSixteenSaySoOnce();
    void repaintingWithTheSameValuesSaysNothing();
    void midnightIsWhatMoleAlreadyLookedLike();
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

/// Sixteen values that share nothing with Midnight, so a token that failed to
/// move can be named rather than merely counted.
Palette::Tokens somethingElseEntirely()
{
    Palette::Tokens t;
    t.window = QColor(QStringLiteral("#f2f3f5"));
    t.panel = QColor(QStringLiteral("#ffffff"));
    t.pane = QColor(QStringLiteral("#fdfdfd"));
    t.border = QColor(QStringLiteral("#dfe3e8"));
    t.hover = QColor(QStringLiteral("#eceff3"));
    t.selection = QColor(QStringLiteral("#dbe7f8"));
    t.text = QColor(QStringLiteral("#191c21"));
    t.textSecondary = QColor(QStringLiteral("#414852"));
    t.textMuted = QColor(QStringLiteral("#6a727d"));
    t.textFaint = QColor(QStringLiteral("#949aa4"));
    t.accent = QColor(QStringLiteral("#2f6feb"));
    t.link = QColor(QStringLiteral("#1f5ed6"));
    t.ok = QColor(QStringLiteral("#1f7a3d"));
    t.warn = QColor(QStringLiteral("#96650a"));
    t.bad = QColor(QStringLiteral("#c23a30"));
    t.busy = QColor(QStringLiteral("#eaf1fc"));
    return t;
}

} // namespace

/// The trap the whole scheme turns on, and the only test that can catch it.
///
/// Declare the token properties `CONSTANT` and QML evaluates each binding once:
/// the window keeps whatever the palette held when it loaded, repainting it
/// afterwards does nothing at all, and Qt says nothing about it -- no warning,
/// nothing in the log, every value in the file correct. So this binds through the
/// path a view actually uses, `App.colour.panel`, repaints the palette underneath
/// it, and requires the binding to have moved.
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

    app.colour()->setTokens(somethingElseEntirely());
    QCOMPARE(bound->property("seen").value<QColor>(), somethingElseEntirely().panel);
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

void TestPalette::everyTokenMovesAndTheSixteenSaySoOnce()
{
    Palette palette;
    QSignalSpy changed(&palette, &Palette::changed);

    const Palette::Tokens after = somethingElseEntirely();
    palette.setTokens(after);

    // One signal for the sixteen, because they only ever move together. Sixteen
    // would be fifteen further passes over every binding in the window.
    QCOMPARE(changed.count(), 1);

    // Named one at a time: "a token did not move" is not a lead.
    for (const Token& token : kTokens) {
        const QVariant read = palette.property(token.name);
        QVERIFY2(read.isValid(), token.name);
        QVERIFY2(read.value<QColor>() == after.*(token.field),
            qPrintable(QStringLiteral("%1 is %2, expected %3")
                           .arg(QLatin1String(token.name), read.value<QColor>().name(),
                               (after.*(token.field)).name())));
    }
}

void TestPalette::repaintingWithTheSameValuesSaysNothing()
{
    Palette palette;
    QSignalSpy changed(&palette, &Palette::changed);
    palette.setTokens(Palette::midnight());
    QCOMPARE(changed.count(), 0);
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

MOLE_TEST_MAIN(TestPalette)
#include "tst_Palette.moc"
