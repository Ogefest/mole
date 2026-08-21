#include "ui/Palette.h"

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

} // namespace

Palette::Palette(QObject* parent)
    : QObject(parent)
    , m_tokens(midnight())
{
}

void Palette::setTokens(const Tokens& tokens)
{
    if (sameTokens(m_tokens, tokens))
        return;
    m_tokens = tokens;
    emit changed();
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

} // namespace mole
