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
    };

    const std::array<Shipped, 2> kThemes { {
        { "Midnight", &Palette::midnight },
        { "Slate", &Palette::slate },
    } };

} // namespace

Palette::Palette(QObject* parent)
    : QObject(parent)
    // Derived from the default's name rather than stated a second time: naming
    // midnight() here and reordering the table later would leave a palette whose
    // theme() and tokens disagreed, which nothing would notice.
    , m_tokens(tokensFor(defaultTheme()))
    , m_theme(defaultTheme())
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
        QColor(QStringLiteral("#8c96a5")), // textMuted
        QColor(QStringLiteral("#6c7583")), // textFaint
        QColor(QStringLiteral("#7fbdd1")), // accent
        QColor(QStringLiteral("#9fd0e0")), // link
        QColor(QStringLiteral("#a0c18b")), // ok
        QColor(QStringLiteral("#e1be7c")), // warn
        QColor(QStringLiteral("#cd7d81")), // bad
        QColor(QStringLiteral("#2b3a45")), // busy
    };
}

} // namespace mole
