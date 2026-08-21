#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>

namespace mole {

/// What the window is painted in, in one place.
///
/// The sibling of the type scale on `AppController`: sizes are chosen once so a
/// listing, a preview and a form line up, and colour is chosen once for the same
/// reason. Before this existed, `src/app/ui` held 372 colour literals across 39
/// files and 75 distinct values between them -- three separate families of grey,
/// none of which anybody had chosen. See ADR-0072.
///
/// A view names a token and never a value. Sixteen of them, each with a job, and
/// the name is the contract: a theme may repaint every one of them, and no view
/// changes.
class Palette : public QObject
{
    Q_OBJECT

    /// The tokens. Read-only from QML and every one of them notifies through the
    /// same signal.
    ///
    /// **Not `CONSTANT`, and the distinction costs a theme its entire effect.** A
    /// `CONSTANT` property is evaluated once and never again, so a window bound to
    /// constant tokens keeps whatever the palette held at startup -- silently, with
    /// nothing in the log. The pointer on `AppController` is constant because the
    /// palette itself never changes; what is inside it does.
    Q_PROPERTY(QColor window READ window NOTIFY changed)
    Q_PROPERTY(QColor panel READ panel NOTIFY changed)
    Q_PROPERTY(QColor pane READ pane NOTIFY changed)
    Q_PROPERTY(QColor border READ border NOTIFY changed)
    Q_PROPERTY(QColor hover READ hover NOTIFY changed)
    Q_PROPERTY(QColor selection READ selection NOTIFY changed)
    Q_PROPERTY(QColor text READ text NOTIFY changed)
    Q_PROPERTY(QColor textSecondary READ textSecondary NOTIFY changed)
    Q_PROPERTY(QColor textMuted READ textMuted NOTIFY changed)
    Q_PROPERTY(QColor textFaint READ textFaint NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor link READ link NOTIFY changed)
    Q_PROPERTY(QColor ok READ ok NOTIFY changed)
    Q_PROPERTY(QColor warn READ warn NOTIFY changed)
    Q_PROPERTY(QColor bad READ bad NOTIFY changed)
    Q_PROPERTY(QColor busy READ busy NOTIFY changed)
    /// Which of the shipped themes is in force. Notifies through the same signal
    /// as the tokens, because it only ever moves when they do.
    Q_PROPERTY(QString theme READ theme NOTIFY changed)

public:
    /// One theme's worth of values. Sixteen fields in the order the tokens are
    /// documented, so a theme reads as a table rather than as sixteen assignments.
    struct Tokens
    {
        QColor window; ///< the shell behind everything
        QColor panel; ///< sidebar, toolbar, task strip, dialog grounds
        QColor pane; ///< a file list's own ground
        QColor border; ///< every hairline
        QColor hover; ///< a row or a bar under the pointer
        QColor selection; ///< the current row, the current crumb
        QColor text; ///< a file name, a value
        QColor textSecondary; ///< sizes, dates, column headers
        QColor textMuted; ///< labels, counts, keyboard hints
        QColor textFaint; ///< the floor: placeholder and disabled
        QColor accent; ///< focus, the active pane's border, the active tab
        QColor link; ///< something that can be followed
        QColor ok; ///< a drive that is fine, a check that passed
        QColor warn; ///< a drive nearly full, a dry run with something to say
        QColor bad; ///< a failure, a destructive button
        QColor busy; ///< the ground of a task strip with work in it
    };

    explicit Palette(QObject* parent = nullptr);

    QColor window() const { return m_tokens.window; }
    QColor panel() const { return m_tokens.panel; }
    QColor pane() const { return m_tokens.pane; }
    QColor border() const { return m_tokens.border; }
    QColor hover() const { return m_tokens.hover; }
    QColor selection() const { return m_tokens.selection; }
    QColor text() const { return m_tokens.text; }
    QColor textSecondary() const { return m_tokens.textSecondary; }
    QColor textMuted() const { return m_tokens.textMuted; }
    QColor textFaint() const { return m_tokens.textFaint; }
    QColor accent() const { return m_tokens.accent; }
    QColor link() const { return m_tokens.link; }
    QColor ok() const { return m_tokens.ok; }
    QColor warn() const { return m_tokens.warn; }
    QColor bad() const { return m_tokens.bad; }
    QColor busy() const { return m_tokens.busy; }

    const Tokens& tokens() const { return m_tokens; }

    QString theme() const { return m_theme; }
    /// Repaints the whole window and announces it once. Nothing happens when the
    /// theme asked for is the one already in force.
    ///
    /// Returns false when `name` is not one of `themeNames()`, having painted the
    /// default instead: a preferences file naming a theme that no longer exists
    /// has to open on something rather than on nothing.
    bool setTheme(const QString& name);

    /// The themes Mole ships, in the order they appear in the `View` menu.
    static QStringList themeNames();
    /// The default, and the one the guide's pictures are taken in.
    static QString defaultTheme();
    /// The default's values for a name that is not a theme.
    static Tokens tokensFor(const QString& name);

    /// What Mole has looked like since it had a window.
    static Tokens midnight();
    /// The same window one step cooler and softer. Sixteen different values and
    /// nothing else: no polarity to reconsider, no control that stops working.
    static Tokens slate();

signals:
    /// One signal for all sixteen, because they only ever move together. QML
    /// re-evaluates every binding that touched any of them.
    void changed();

private:
    Tokens m_tokens;
    QString m_theme;
};

} // namespace mole
