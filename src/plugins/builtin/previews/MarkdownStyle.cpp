#include "plugins/builtin/previews/MarkdownStyle.h"

#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

#include <algorithm>
#include <cmath>

namespace mole {
namespace {

    /// How much room a heading gets and how big it is, as multiples of the body
    /// size. The gap above is far larger than the gap below on purpose: a heading
    /// belongs to the text underneath it, and spacing is what says so.
    struct HeadingScale
    {
        double spaceAbove;
        double spaceBelow;
        double size;
    };

    constexpr HeadingScale kHeadings[6] = {
        { 1.70, 0.60, 1.90 },
        { 1.55, 0.50, 1.55 },
        { 1.35, 0.45, 1.30 },
        { 1.20, 0.40, 1.14 },
        { 1.10, 0.35, 1.02 },
        { 1.10, 0.35, 0.94 },
    };

    /// Qt indents each level of a quote by this much, and that margin is the only
    /// evidence of how deeply the quote was nested.
    constexpr double kImporterQuoteIndent = 40.0;

    /// So the depth survives being restyled: the moment the importer's margin is
    /// replaced with our own, the evidence above is gone. Recording it keeps a
    /// second pass from reading its own output as a different nesting.
    constexpr int kQuoteDepthProperty = QTextFormat::UserProperty + 1;

    enum class BlockKind { Paragraph, Heading, CodeBlock, ListItem, Quote, HorizontalRule, TableCell };

    int scaled(int body, double ratio)
    {
        return static_cast<int>(std::lround(body * ratio));
    }

    bool isMonospace(const QTextCharFormat& format)
    {
        if (format.fontFixedPitch())
            return true;
        const QStringList families = format.fontFamilies().toStringList();
        return std::any_of(families.cbegin(), families.cend(), [](const QString& family) {
            return family.compare(QLatin1String("monospace"), Qt::CaseInsensitive) == 0;
        });
    }

    /// Unbreakable lines, and nothing else. A monospace block font looks like a
    /// tempting second signal and is a trap: a paragraph that merely opens with a
    /// `code span` gets one from the importer, and it would be handed a slab of its
    /// own. Indented code blocks need no second signal either -- this Markdown
    /// dialect does not recognise them, so they arrive as ordinary paragraphs and
    /// there is nothing here to find.
    bool isCodeBlock(const QTextBlock& block)
    {
        return block.blockFormat().nonBreakableLines();
    }

    BlockKind kindOf(const QTextBlock& block)
    {
        const QTextBlockFormat format = block.blockFormat();
        if (format.hasProperty(QTextFormat::BlockTrailingHorizontalRulerWidth))
            return BlockKind::HorizontalRule;
        if (format.headingLevel() > 0)
            return BlockKind::Heading;
        if (isCodeBlock(block))
            return BlockKind::CodeBlock;
        if (block.textList())
            return BlockKind::ListItem;
        if (QTextCursor(block).currentTable())
            return BlockKind::TableCell;
        if (format.leftMargin() > 0 || format.hasProperty(kQuoteDepthProperty))
            return BlockKind::Quote;
        return BlockKind::Paragraph;
    }

    bool isKind(const QTextBlock& block, BlockKind kind)
    {
        return block.isValid() && kindOf(block) == kind;
    }

    /// Sets a size in pixels, which means clearing the two ways the importer sizes
    /// text: code arrives in points and headings by adjustment, and either one left
    /// in place would fight the pixel size rather than lose to it.
    void setPixelSize(QTextCharFormat& format, int pixels)
    {
        format.clearProperty(QTextFormat::FontPointSize);
        format.clearProperty(QTextFormat::FontSizeAdjustment);
        format.setProperty(QTextFormat::FontPixelSize, pixels);
    }

    /// Rewrites the character formats of one block, fragment by fragment, so that
    /// bold, links and inline code survive. Fragment by fragment rather than one
    /// merge over the block because a merge cannot remove a property, and the sizes
    /// the importer leaves behind have to go.
    template<typename Tweak>
    void restyleText(QTextCursor& cursor, const QTextBlock& block, const Tweak& tweak)
    {
        for (QTextBlock::iterator it = block.begin(); it != block.end(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            QTextCharFormat format = fragment.charFormat();
            tweak(format);
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            cursor.setCharFormat(format);
        }

        // The block's own format decides the height of an empty line and the metrics
        // the layout starts from, so it gets the same treatment.
        QTextCharFormat format = block.charFormat();
        tweak(format);
        cursor.setPosition(block.position());
        cursor.setBlockCharFormat(format);
    }

    /// Inline `code` inside prose: the importer sets it to nine points, which next
    /// to body text reads as a mistake. Sized against the text around it instead,
    /// and given the same slab colour as a code block so the two are recognisably
    /// the same thing.
    void styleInlineCode(
        QTextCursor& cursor, const QTextBlock& block, const MarkdownStyle::Metrics& metrics, int size)
    {
        for (QTextBlock::iterator it = block.begin(); it != block.end(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            QTextCharFormat format = fragment.charFormat();
            if (!isMonospace(format))
                continue;

            format.setFontFamilies({ metrics.monospaceFamily });
            // Set explicitly, because the family above is no longer literally
            // "monospace" and this is what a second pass recognises it by.
            format.setFontFixedPitch(true);
            setPixelSize(format, size);
            if (metrics.codeBackground.isValid())
                format.setBackground(metrics.codeBackground);

            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            cursor.setCharFormat(format);
        }
    }

    /// Tables, which are frames rather than blocks. Qt gives them no cell padding
    /// at all, so the text touches the rules on every side.
    void styleTables(QTextFrame* frame, const MarkdownStyle::Metrics& metrics, int padding, int gap)
    {
        const QList<QTextFrame*> children = frame->childFrames();
        for (QTextFrame* child : children) {
            if (auto* table = qobject_cast<QTextTable*>(child)) {
                QTextTableFormat format = table->format();
                format.setCellPadding(padding);
                format.setCellSpacing(0);
                format.setBorder(1);
                format.setBorderCollapse(true);
                if (metrics.rule.isValid())
                    format.setBorderBrush(metrics.rule);
                format.setTopMargin(gap);
                format.setBottomMargin(gap);
                table->setFormat(format);
            }
            // A table can hold a table, so this is not a single pass over the root.
            styleTables(child, metrics, padding, gap);
        }
    }

} // namespace

MarkdownStyle::MarkdownStyle(QObject* parent)
    : QObject(parent)
{
}

void MarkdownStyle::setMetrics(const Metrics& metrics)
{
    m_metrics = metrics;
    reapply();
}

void MarkdownStyle::attachTo(QQuickTextDocument* document)
{
    attachTo(document ? document->textDocument() : nullptr);
}

void MarkdownStyle::detach()
{
    attachTo(static_cast<QTextDocument*>(nullptr));
}

void MarkdownStyle::attachTo(QTextDocument* target)
{
    // Guarded, because attaching restyles, which changes the document, which is
    // exactly what a caller reacting to "the text changed" is responding to.
    if (target == m_document)
        return;

    if (m_document)
        disconnect(m_document, &QTextDocument::contentsChanged, this, &MarkdownStyle::scheduleReapply);

    // Whatever the last document asked for is no longer wanted: a pass that
    // arrived after the file changed would style this one on the last one's
    // behalf, and if it were detached entirely it would style nothing at all.
    m_restylePending = false;

    m_document = target;
    if (!m_document)
        return;

    connect(m_document, &QTextDocument::contentsChanged, this, &MarkdownStyle::scheduleReapply);
    // Directly, not queued: whatever is in the document now is finished, and
    // there is no reason for the reader to see one frame of it unstyled.
    reapply();
}

void MarkdownStyle::scheduleReapply()
{
    if (m_applying || m_restylePending || !m_document)
        return;

    m_restylePending = true;
    // Queued, so it runs once the event loop turns and whoever was editing the
    // document has finished with it -- the importer included, which announces
    // each of its own insertions from inside the edit that made it.
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (!m_restylePending)
                return;
            m_restylePending = false;
            reapply();
        },
        Qt::QueuedConnection);
}

void MarkdownStyle::reapply()
{
    if (m_applying || !m_document)
        return;

    m_applying = true;
    applyTo(m_document, m_metrics);
    m_applying = false;
}

void MarkdownStyle::applyTo(QTextDocument* document, const Metrics& metrics)
{
    if (!document)
        return;

    const int body = std::max(1, metrics.bodyPixelSize);
    const int paragraphGap = scaled(body, 0.85);
    const int codeSize = std::max(1, scaled(body, 0.92));
    const int codeInset = scaled(body, 0.80);
    const int quoteIndent = scaled(body, 1.60);
    const int quoteGap = scaled(body, 0.35);
    const int listGap = scaled(body, 0.30);
    const int ruleGap = scaled(body, 1.40);
    const int cellPadding = scaled(body, 0.45);

    // A preview is read-only, so there is nothing to undo -- and a document of
    // ten thousand blocks would otherwise keep a format command for every one.
    const bool undoWasEnabled = document->isUndoRedoEnabled();
    document->setUndoRedoEnabled(false);

    QTextCursor cursor(document);
    cursor.beginEditBlock();

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const BlockKind kind = kindOf(block);
        /// Kept because the margins below are about to be overwritten, and one of
        /// them is the only record of how deeply a quote was nested.
        const QTextBlockFormat asImported = block.blockFormat();
        QTextBlockFormat format = asImported;
        /// Inline code scales with the text around it, which in a heading is
        /// not the body size.
        int inlineCodeSize = codeSize;

        // Absolute values throughout, never increments: this runs again on every
        // change to the document, including the changes it makes itself.
        format.setTopMargin(0);
        format.setBottomMargin(paragraphGap);
        format.setLeftMargin(0);
        format.setRightMargin(0);
        format.setLineHeight(155, QTextBlockFormat::ProportionalHeight);
        format.clearBackground();

        switch (kind) {
        case BlockKind::Heading: {
            const HeadingScale& scale = kHeadings[std::clamp(format.headingLevel(), 1, 6) - 1];
            const int level = std::clamp(format.headingLevel(), 1, 6);
            const int size = scaled(body, scale.size);
            inlineCodeSize = std::max(1, scaled(size, 0.92));
            // The first block sits against the top of the page, where the view's
            // own padding already provides the space.
            format.setTopMargin(block.blockNumber() == 0 ? 0 : scaled(body, scale.spaceAbove));
            format.setBottomMargin(scaled(body, scale.spaceBelow));
            format.setLineHeight(125, QTextBlockFormat::ProportionalHeight);

            restyleText(cursor, block, [&](QTextCharFormat& text) {
                setPixelSize(text, size);
                // Qt leaves a level-one heading at normal weight, which reads as
                // a mistake next to the levels below it.
                text.setFontWeight(level <= 3 ? QFont::Bold : QFont::DemiBold);
                // The smallest heading is a label rather than a title, and the
                // size alone is too small a difference to notice.
                if (level == 6 && metrics.mutedText.isValid())
                    text.setForeground(metrics.mutedText);
            });
            break;
        }

        case BlockKind::CodeBlock: {
            // A fence arrives as one block per line, so the slab is a run of
            // them: only the first gets space above it and only the last below,
            // or the band would be broken up by its own margins.
            const bool firstOfRun = !isKind(block.previous(), BlockKind::CodeBlock);
            const bool lastOfRun = !isKind(block.next(), BlockKind::CodeBlock);
            format.setTopMargin(firstOfRun ? paragraphGap : 0);
            format.setBottomMargin(lastOfRun ? paragraphGap : 0);
            format.setLeftMargin(codeInset);
            format.setRightMargin(codeInset);
            // Set solid, unlike the prose around it, and not for want of room:
            // the background that makes the slab visible is painted behind the
            // glyphs, and any extra leading falls outside it. Loosening the
            // lines here does not pad the slab, it cuts it into stripes.
            format.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
            // On the block as well as the text below, because the two rendering
            // paths disagree about which one they honour: a QPainter draws the
            // block's background, and the scene graph behind a QML TextArea --
            // where this is actually seen -- draws only the text's.
            if (metrics.codeBackground.isValid())
                format.setBackground(metrics.codeBackground);

            restyleText(cursor, block, [&](QTextCharFormat& text) {
                text.setFontFamilies({ metrics.monospaceFamily });
                text.setFontFixedPitch(true);
                setPixelSize(text, codeSize);
                if (metrics.codeBackground.isValid())
                    text.setBackground(metrics.codeBackground);
            });
            break;
        }

        case BlockKind::ListItem: {
            // Tight between items, because a list is one thing; the space
            // belongs around the list, not inside it.
            const QTextList* list = block.textList();
            const bool lastOfList = !block.next().isValid() || block.next().textList() != list;
            format.setBottomMargin(lastOfList ? paragraphGap : listGap);
            format.setLineHeight(150, QTextBlockFormat::ProportionalHeight);
            break;
        }

        case BlockKind::Quote: {
            const int depth = asImported.hasProperty(kQuoteDepthProperty)
                ? asImported.intProperty(kQuoteDepthProperty)
                : std::max(1, static_cast<int>(std::lround(asImported.leftMargin() / kImporterQuoteIndent)));
            format.setProperty(kQuoteDepthProperty, depth);

            const bool firstOfRun = !isKind(block.previous(), BlockKind::Quote);
            const bool lastOfRun = !isKind(block.next(), BlockKind::Quote);
            format.setLeftMargin(depth * quoteIndent);
            format.setRightMargin(codeInset);
            format.setTopMargin(firstOfRun ? quoteGap : 0);
            format.setBottomMargin(lastOfRun ? paragraphGap : quoteGap);

            // Nothing draws a border down the side of a block in Qt's rich text,
            // so the indent and a quieter colour are what mark a quote.
            if (metrics.mutedText.isValid()) {
                restyleText(
                    cursor, block, [&](QTextCharFormat& text) { text.setForeground(metrics.mutedText); });
            }
            break;
        }

        case BlockKind::HorizontalRule:
            format.setTopMargin(ruleGap);
            format.setBottomMargin(ruleGap);
            break;

        case BlockKind::TableCell:
            // Cell padding does this job; margins here would double it.
            format.setTopMargin(0);
            format.setBottomMargin(0);
            format.setLineHeight(135, QTextBlockFormat::ProportionalHeight);
            break;

        case BlockKind::Paragraph:
            break;
        }

        cursor.setPosition(block.position());
        cursor.setBlockFormat(format);

        if (kind != BlockKind::CodeBlock)
            styleInlineCode(cursor, block, metrics, inlineCodeSize);
    }

    styleTables(document->rootFrame(), metrics, cellPadding, paragraphGap);

    cursor.endEditBlock();
    document->setUndoRedoEnabled(undoWasEnabled);
}

} // namespace mole
