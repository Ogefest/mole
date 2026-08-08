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
        /// Defaults match the application's dark palette; the view can pass its
        /// own, because the palette belongs to the view.
        QColor codeBackground = QColor(QStringLiteral("#232a36"));
        QColor mutedText = QColor(QStringLiteral("#9aa3b5"));
        QColor rule = QColor(QStringLiteral("#39414f"));
    };

    explicit MarkdownStyle(QObject* parent = nullptr);

    void setMetrics(const Metrics& metrics);
    Metrics metrics() const { return m_metrics; }

    /// Styles this document now, and again whenever its contents change.
    /// Restyling is not optional: setting a TextArea's text re-imports the
    /// Markdown, which throws away everything done here. Pass nullptr to
    /// detach, which the viewer does when the next file is not Markdown.
    void attachTo(QQuickTextDocument* document);

    /// Idempotent: applying it twice leaves the same document, so it can run on
    /// every change without margins piling up.
    static void applyTo(QTextDocument* document, const Metrics& metrics);

private:
    void reapply();

    Metrics m_metrics;
    QPointer<QTextDocument> m_document;
    /// Styling changes the document, which is what brought us here. Without
    /// this the first change would recurse.
    bool m_applying = false;
};

} // namespace mole
