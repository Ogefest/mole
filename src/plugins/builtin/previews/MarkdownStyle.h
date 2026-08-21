#pragma once

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QString>

class QQuickTextDocument;
class QTextDocument;

namespace mole {

/// Typography for a rendered Markdown document.
///
/// Qt's Markdown importer produces a document that is correct and cramped.
/// Headings get no space above or below them at all, so a section title sits
/// flush against the paragraph it belongs to; every line is set solid; and a
/// fenced code block arrives as nine-point monospace with no margins and
/// nothing behind it. A Markdown file exists to be read as prose, and prose
/// needs room.
///
/// This walks the document after the import and gives it that room. It works on
/// the real QTextDocument rather than through a style sheet because a style
/// sheet only reaches a document built by the HTML importer -- setMarkdown()
/// never consults one -- and it changes formats only, never structure, so the
/// text a reader selects and copies is still the text that was in the file.
/// See docs/adr/0001-markdown-preview-typography.md.
class MarkdownStyle final : public QObject
{
    Q_OBJECT

public:
    /// What the styling is derived from: one text size, one monospace family,
    /// and the few colours that are not simply the body colour. Everything else
    /// is a ratio of the body size, so moving that one number moves the whole
    /// page in proportion.
    struct Metrics
    {
        int bodyPixelSize = 15;
        QString monospaceFamily = QStringLiteral("monospace");
        /// The slab behind code, the colour of quoted text and of table rules.
        ///
        /// **These three are the window's paint rather than the document's**, and
        /// the view passes them from the palette -- `hover`, `textMuted` and
        /// `border` -- because it is the view that knows what the window is
        /// painted in. They were stated here as dark values until MOLE-280, which
        /// made a light theme possible and turned the code slab into a dark
        /// rectangle in the middle of a white page. The defaults are what Midnight
        /// passes, so a caller that sets nothing still gets a readable page. See
        /// ADR-0074.
        QColor codeBackground = QColor(QStringLiteral("#232a36"));
        QColor mutedText = QColor(QStringLiteral("#8b93a7"));
        QColor rule = QColor(QStringLiteral("#2a3140"));
    };

    explicit MarkdownStyle(QObject* parent = nullptr);

    void setMetrics(const Metrics& metrics);
    Metrics metrics() const { return m_metrics; }

    /// Styles this document now, and again whenever its contents change.
    /// Restyling is not optional: setting a TextArea's text re-imports the
    /// Markdown, which throws away everything done here.
    ///
    /// The restyling that follows a change is deferred to the next turn of the
    /// event loop rather than done where the change was announced. Qt's
    /// Markdown importer emits `contentsChanged` from inside its own
    /// `insertText`, many times, while the document is still being built --
    /// walking it there means reading blocks and fragments that do not exist
    /// yet.
    void attachTo(QQuickTextDocument* document);
    /// The same, on the document itself. What the class actually works on, and
    /// what a test can hand it without a window.
    void attachTo(QTextDocument* document);
    /// Leaves the document it was on alone from here, which the viewer does
    /// when the next file is not Markdown. A named call rather than a null
    /// argument, because with two overloads a bare nullptr means neither.
    void detach();

    /// Idempotent: applying it twice leaves the same document, so it can run on
    /// every change without margins piling up.
    static void applyTo(QTextDocument* document, const Metrics& metrics);

private:
    /// Asks for a restyle on the next turn of the event loop, at most one
    /// pending at a time. The importer makes an edit per piece of text it
    /// parses, so an immediate pass would run hundreds of times over a document
    /// that is not finished -- and the first of them can be fatal.
    void scheduleReapply();
    void reapply();

    Metrics m_metrics;
    QPointer<QTextDocument> m_document;
    /// Styling changes the document, which is what brought us here. Without
    /// this the first change would recurse.
    bool m_applying = false;
    /// A restyle is queued. Cleared when the document is detached or replaced,
    /// so a pass asked for by the last file never lands on the next one.
    bool m_restylePending = false;
};

} // namespace mole
