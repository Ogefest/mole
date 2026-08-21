#include "ui/Palette.h"

#include <QLoggingCategory>

#include <array>

namespace mole {

namespace {

    bool sameTokens(const Palette::Tokens& a, const Palette::Tokens& b)
    {
        return a.window == b.window && a.panel == b.panel && a.pane == b.pane && a.border == b.border
            && a.hover == b.hover && a.selection == b.selection && a.text == b.text
            && a.textSecondary == b.textSecondary && a.textMuted == b.textMuted && a.textFaint == b.textFaint
            && a.accent == b.accent && a.link == b.link && a.ok == b.ok && a.warn == b.warn && a.bad == b.bad
            && a.busy == b.busy;
    }

    /// The shipped themes, in the order the `View` menu offers them. One entry per
    /// theme and one table per entry -- adding a theme is a function and a line here,
    /// and touches no view.
    struct Shipped
    {
        const char* name;
        Palette::Tokens (*tokens)();
        /// Stated, not derived. See the note on Palette::light.
        bool light;
    };

    const std::array<Shipped, 4> kThemes { {
        { "Midnight", &Palette::midnight, false },
        { "Slate", &Palette::slate, false },
        { "Paper", &Palette::paper, true },
        { "Workbench", &Palette::workbench, true },
    } };

} // namespace

Palette::Palette(QObject* parent)
    : QObject(parent)
    // Derived from the default's name rather than stated a second time: naming
    // midnight() here and reordering the table later would leave a palette whose
    // theme() and tokens disagreed, which nothing would notice.
    , m_tokens(tokensFor(defaultTheme()))
    , m_theme(defaultTheme())
    , m_light(isLightTheme(defaultTheme()))
{
}

QStringList Palette::themeNames()
{
    QStringList names;
    names.reserve(static_cast<int>(kThemes.size()));
    for (const Shipped& theme : kThemes)
        names.append(QString::fromLatin1(theme.name));
    return names;
}

QString Palette::defaultTheme()
{
    return QString::fromLatin1(kThemes.front().name);
}

Palette::Tokens Palette::tokensFor(const QString& name)
{
    for (const Shipped& theme : kThemes) {
        if (name == QLatin1String(theme.name))
            return theme.tokens();
    }
    return kThemes.front().tokens();
}

bool Palette::isLightTheme(const QString& name)
{
    for (const Shipped& theme : kThemes) {
        if (name == QLatin1String(theme.name))
            return theme.light;
    }
    return kThemes.front().light;
}

bool Palette::setTheme(const QString& name)
{
    const bool known = themeNames().contains(name);
    // Said rather than swallowed. A preference naming a theme that is not there
    // is either a file from a newer build or a typo somebody made by hand, and
    // both are worth one line in the log; opening on nothing is not an option.
    if (!known && !name.isEmpty())
        qWarning("No theme called %s; opening on %s", qPrintable(name), qPrintable(defaultTheme()));

    const QString settled = known ? name : defaultTheme();
    const Tokens wanted = tokensFor(settled);
    if (settled == m_theme && sameTokens(m_tokens, wanted))
        return known;

    m_theme = settled;
    m_tokens = wanted;
    m_light = isLightTheme(settled);
    emit changed();
    return known;
}

Palette::Tokens Palette::midnight()
{
    // Every value here was already in the tree, in one QML file or another, before
    // there was anywhere to put it. Nothing was invented at this point.
    return Tokens {
        QColor(QStringLiteral("#151922")), // window
        QColor(QStringLiteral("#1b2029")), // panel
        QColor(QStringLiteral("#151922")), // pane
        QColor(QStringLiteral("#2a3140")), // border
        QColor(QStringLiteral("#232a36")), // hover
        QColor(QStringLiteral("#26303f")), // selection
        QColor(QStringLiteral("#e6ebf5")), // text
        QColor(QStringLiteral("#c9d1e0")), // textSecondary
        QColor(QStringLiteral("#8b93a7")), // textMuted
        QColor(QStringLiteral("#6f7788")), // textFaint
        QColor(QStringLiteral("#4c9aff")), // accent
        QColor(QStringLiteral("#7cc4ff")), // link
        QColor(QStringLiteral("#57ab5a")), // ok
        QColor(QStringLiteral("#d9a441")), // warn
        QColor(QStringLiteral("#e5534b")), // bad
        QColor(QStringLiteral("#1e2a3a")), // busy
    };
}

Palette::Tokens Palette::slate()
{
    return Tokens {
        QColor(QStringLiteral("#232830")), // window
        QColor(QStringLiteral("#2a3038")), // panel
        QColor(QStringLiteral("#1e232a")), // pane
        QColor(QStringLiteral("#383f4b")), // border
        QColor(QStringLiteral("#303845")), // hover
        QColor(QStringLiteral("#3a4657")), // selection
        QColor(QStringLiteral("#dde4ed")), // text
        QColor(QStringLiteral("#c0c9d5")), // textSecondary
        QColor(QStringLiteral("#97a2b2")), // textMuted
        QColor(QStringLiteral("#6c7583")), // textFaint
        QColor(QStringLiteral("#7fbdd1")), // accent
        QColor(QStringLiteral("#9fd0e0")), // link
        QColor(QStringLiteral("#a0c18b")), // ok
        QColor(QStringLiteral("#e1be7c")), // warn
        QColor(QStringLiteral("#cd7d81")), // bad
        QColor(QStringLiteral("#2b3a45")), // busy
    };
}

Palette::Tokens Palette::paper()
{
    // Flat: panel and pane are both white and the shell behind them is a shade
    // off it, so the window reads as one sheet with things drawn on it.
    return Tokens {
        QColor(QStringLiteral("#f2f3f5")), // window
        QColor(QStringLiteral("#ffffff")), // panel
        QColor(QStringLiteral("#ffffff")), // pane
        QColor(QStringLiteral("#dfe3e8")), // border
        QColor(QStringLiteral("#eceff3")), // hover
        QColor(QStringLiteral("#dbe7f8")), // selection
        QColor(QStringLiteral("#191c21")), // text
        QColor(QStringLiteral("#414852")), // textSecondary
        QColor(QStringLiteral("#666d78")), // textMuted
        QColor(QStringLiteral("#949aa4")), // textFaint
        QColor(QStringLiteral("#2f6feb")), // accent
        QColor(QStringLiteral("#1f5ed6")), // link
        QColor(QStringLiteral("#1f7a3d")), // ok
        QColor(QStringLiteral("#96650a")), // warn
        QColor(QStringLiteral("#c23a30")), // bad
        QColor(QStringLiteral("#eaf1fc")), // busy
    };
}

Palette::Tokens Palette::workbench()
{
    // The other answer to the same polarity: the chrome is a visible grey and
    // pure white is kept for the listing, so a pane reads as a pane without
    // relying on the focus ring to say where it is.
    return Tokens {
        QColor(QStringLiteral("#e6e9ec")), // window
        QColor(QStringLiteral("#f2f4f6")), // panel
        QColor(QStringLiteral("#ffffff")), // pane
        QColor(QStringLiteral("#c8ced6")), // border
        QColor(QStringLiteral("#dde2e8")), // hover
        QColor(QStringLiteral("#cde6ea")), // selection
        QColor(QStringLiteral("#10151a")), // text
        QColor(QStringLiteral("#39414b")), // textSecondary
        QColor(QStringLiteral("#5e6873")), // textMuted
        QColor(QStringLiteral("#8d97a3")), // textFaint
        QColor(QStringLiteral("#0f6c80")), // accent
        QColor(QStringLiteral("#0b5a6b")), // link
        QColor(QStringLiteral("#2c7a3f")), // ok
        QColor(QStringLiteral("#9a6508")), // warn
        QColor(QStringLiteral("#b6362c")), // bad
        QColor(QStringLiteral("#dff0f3")), // busy
    };
}

} // namespace mole
