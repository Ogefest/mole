#include "plugins/builtin/previews/PreviewProviders.h"

#include "core/platform/Staging.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QLocale>
#include <QMimeDatabase>
#include <QQuickTextDocument>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUrl>

#include <algorithm>

namespace mole {
namespace {

    QUrl qmlView(const char* name)
    {
        return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/%1").arg(QLatin1String(name)));
    }

    /// Everything Qt's text engine starts a new block on. Measured against the
    /// same set the engine uses, so a file with old Mac endings -- or one that
    /// is already in blocks for a reason of its own -- is not folded for nothing.
    bool isLineBreak(QChar c)
    {
        return c == u'\n' || c == u'\r' || c == u'\v' || c == QChar::LineSeparator
            || c == QChar::ParagraphSeparator;
    }

    /// Whether any run without a line break in it is longer than `limit`.
    /// Separate from the fold so the ordinary file costs one scan and no
    /// allocation: almost every window read is this and nothing else.
    bool hasOverlongLine(const QString& text, qsizetype limit)
    {
        qsizetype run = 0;
        for (const QChar c : text) {
            if (isLineBreak(c))
                run = 0;
            else if (++run > limit)
                return true;
        }
        return false;
    }

    /// The longest run of consecutive lines that begin with a table's pipe.
    ///
    /// A GFM table row starts with `|`, and so does the `|---|` rule under the
    /// header, so a run is the header, the rule and the body counted together.
    /// Leading whitespace is skipped, because a table inside a list item is
    /// indented, and nothing else about the line is looked at: this is a cost
    /// estimate and not a parse. Qt's importer decides what a table is; the only
    /// question here is roughly how many rows it is about to be handed.
    ///
    /// One pass and no allocation, which is the same trade hasOverlongLine()
    /// makes beside it -- a scan of a quarter of a megabyte against an import
    /// that can take seconds means the ordinary file pays almost nothing.
    qsizetype longestTableRun(const QString& text)
    {
        qsizetype longest = 0;
        qsizetype run = 0;
        qsizetype i = 0;
        while (i < text.size()) {
            qsizetype end = i;
            while (end < text.size() && !isLineBreak(text.at(end)))
                ++end;

            qsizetype first = i;
            while (first < end && text.at(first).isSpace())
                ++first;

            if (first < end && text.at(first) == u'|')
                longest = std::max(longest, ++run);
            else
                run = 0;

            // A CRLF ends one line, not two.
            i = end;
            if (i < text.size() && text.at(i) == u'\r')
                ++i;
            if (i < text.size() && isLineBreak(text.at(i)))
                ++i;
        }
        return longest;
    }

    /// Breaks every run longer than `limit` into pieces of `limit` characters.
    /// The file's line numbers, one per row of `text` as a document lays it out.
    ///
    /// Counting `\n` and nothing else. isLineBreak() is wider on purpose -- it
    /// decides where a *fold* may go -- but a number in a gutter has to agree with
    /// every other tool the reader owns, and those count newlines.
    QList<int> lineNumbersFor(const QString& text)
    {
        QList<int> rows;
        rows.reserve(text.count(u'\n') + 1);
        int number = 1;
        rows.append(number);
        for (const QChar c : text) {
            if (c == u'\n')
                rows.append(++number);
        }
        // A file ending in a newline has one row after it with nothing in it, and
        // that row is not a line of the file: `wc -l` says forty for forty lines
        // however the last one ends, and a gutter that said forty-one would be
        // arguing with the tool the reader checked it against.
        if (text.endsWith(u'\n') && rows.size() > 1)
            rows.last() = 0;
        return rows;
    }

    /// Folds over-long runs, and says which file line each resulting row belongs
    /// to when `rowsOut` is given.
    ///
    /// **A folded run is one line of the file shown as several rows**, so the
    /// rows it was broken into carry 0 rather than a number of their own: a gutter
    /// numbering rows would disagree with the compiler, the stack trace and the
    /// reader's editor, which is the same reason the fold exists at all. Reported
    /// from in here because this is the only place that knows which breaks it put
    /// in and which were already there.
    QString withLongLinesFolded(const QString& text, qsizetype limit, QList<int>* rowsOut = nullptr)
    {
        QString folded;
        folded.reserve(text.size() + text.size() / limit + 1);
        int number = 1;
        if (rowsOut) {
            rowsOut->clear();
            rowsOut->append(number);
        }

        qsizetype run = 0;
        for (const QChar c : text) {
            if (isLineBreak(c)) {
                folded.append(c);
                run = 0;
                if (rowsOut && c == u'\n')
                    rowsOut->append(++number);
                continue;
            }
            if (run >= limit) {
                // Never between the halves of a surrogate pair -- that would put
                // an unpaired code unit either side of the fold and show two
                // replacement characters where the file has one emoji. The pair
                // goes on to the next line whole.
                if (!folded.isEmpty() && folded.back().isHighSurrogate()) {
                    const QChar high = folded.back();
                    folded.chop(1);
                    folded.append(u'\n');
                    folded.append(high);
                    run = 1;
                } else {
                    folded.append(u'\n');
                    run = 0;
                }
                // A row this fold made, and not a line of the file.
                if (rowsOut)
                    rowsOut->append(0);
            }
            folded.append(c);
            ++run;
        }
        // The same trailing row as above: what a final newline leaves behind is
        // not a line.
        if (rowsOut && text.endsWith(u'\n') && rowsOut->size() > 1)
            rowsOut->last() = 0;
        return folded;
    }

} // namespace

// ------------------------------------------------------------- local copy

LocalCopyProvider::LocalCopyProvider(PluginServices services, QObject* parent)
    : QObject(parent)
    , m_services(services)
{
}

LocalCopyProvider::~LocalCopyProvider()
{
    cancel();
}

void LocalCopyProvider::cancel()
{
    if (m_task) {
        m_task->requestCancel();
        m_task.clear();
    }
}

void LocalCopyProvider::request(const VfsUri& uri, qint64 maxBytes)
{
    cancel();

    const QString localPath = uri.toLocalPath();
    if (!localPath.isEmpty()) {
        emit ready(QUrl::fromLocalFile(localPath).toString());
        return;
    }

    if (!m_services.isValid()) {
        emit failed(QStringLiteral("Application services are not available"));
        return;
    }

    FileSystemPtr fs = m_services.vfs->resolve(uri);
    if (!fs) {
        emit failed(QStringLiteral("No drive is mounted for this file"));
        return;
    }

    if (!m_scratch) {
        QString why;
        m_scratch = staging::makeDirectory(&why);
        if (!m_scratch) {
            emit failed(QStringLiteral("Cannot make a scratch directory: %1").arg(why));
            return;
        }
    }

    // The name is kept so the image loader can still pick a decoder by suffix.
    const QString target = QDir(m_scratch->path()).filePath(uri.fileName());

    auto* task = new ReadFileTask(std::move(fs), uri, maxBytes);
    m_task = task;
    connect(task, &Task::finished, this, [this, task, target] {
        if (m_task != task)
            return;
        m_task.clear();

        if (task->state() != Task::State::Succeeded) {
            emit failed(task->error().message);
            return;
        }

        QFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit failed(QStringLiteral("Cannot write the extracted copy"));
            return;
        }
        file.write(task->contents());
        file.close();

        emit ready(QUrl::fromLocalFile(target).toString());
    });

    m_services.tasks->submit(task);
}

// ------------------------------------------------------------------ text

TextPreviewController::TextPreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
    , m_services(services)
    , m_highlighter(new SourceHighlighter(this))
    , m_markdownStyle(new MarkdownStyle(this))
{
}

TextPreviewController::~TextPreviewController()
{
    if (m_task)
        m_task->requestCancel();
}

QString TextPreviewController::languageName() const
{
    const SourceHighlighter::Rules* rules = SourceHighlighter::rulesFor(m_language);
    return rules ? rules->displayName : QString();
}

QString TextPreviewController::sizeText() const
{
    return m_fileSize >= 0 ? QLocale().formattedDataSize(m_fileSize) : QString();
}

QString TextPreviewController::positionText() const
{
    if (m_fileSize <= 0)
        return {};
    if (!isPaged())
        return sizeText();

    const QLocale locale;
    return QStringLiteral("%1 – %2 of %3")
        .arg(locale.formattedDataSize(m_windowOffset),
            locale.formattedDataSize(m_windowOffset + m_windowBytes), locale.formattedDataSize(m_fileSize));
}

void TextPreviewController::setViewerOption(const QString& key, const QString& value)
{
    if (key != QLatin1String("mode"))
        return;

    const bool render = value.compare(QLatin1String("Rendered"), Qt::CaseInsensitive) == 0;
    const MarkdownMode mode = render ? MarkdownMode::Rendered : MarkdownMode::Source;
    if (render == m_renderHtml && mode == m_markdownMode)
        return;

    // Both, unconditionally. This arrives before load(), so nothing here knows
    // yet which kind of markup the file is -- and it does not need to: each
    // member is read only for the kind it belongs to, since isRenderedHtml()
    // requires a renderable suffix and the Markdown mode is only consulted for a
    // Markdown file. One question was asked and one answer given; it lands in
    // both places and one of them uses it.
    m_renderHtml = render;
    m_markdownMode = mode;

    // The text already read is reused: switching between source and page is a
    // question about the same bytes, not a reason to go back to the drive.
    updateDisplayText();
    applyViewers();
    emit textChanged();
}

QString TextPreviewController::markdownDeclinedNote() const
{
    if (!m_markdownDeclined)
        return {};

    // The figure, because it is the answer to the only question a reader has
    // here: why this file and not the last one. And "shown as source" rather
    // than a word about tables or milliseconds -- what they need to know is what
    // they are looking at.
    return QStringLiteral("shown as source: a %1-row table would take seconds to render")
        .arg(QLocale().toString(static_cast<qlonglong>(m_markdownTableRows)));
}

QString TextPreviewController::withoutExternalReferences(const QString& html)
{
    // Blunt on purpose. Qt's rich text engine resolves what a document names, so a
    // page could tell whoever wrote it that a file had been looked at -- and in a
    // file manager that is a nasty surprise, not a feature. Telling a local
    // reference from a remote one means parsing and resolving, and getting that
    // subtly wrong is the failure this exists to prevent. See ADR-0006.
    static const QRegularExpression tags(
        QStringLiteral("<\\s*(img|script|link|iframe|object|embed|source|audio|video)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression closers(
        QStringLiteral("<\\s*/\\s*(script|iframe|object|embed|audio|video)\\s*>"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression handlers(
        QStringLiteral("\\son[a-z]+\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
        QRegularExpression::CaseInsensitiveOption);

    QString safe = html;
    safe.remove(tags);
    safe.remove(closers);
    safe.remove(handlers);
    return safe;
}

void TextPreviewController::attachDocument(
    QQuickTextDocument* document, int bodyPixelSize, const QString& monospaceFamily)
{
    m_document = document;

    MarkdownStyle::Metrics metrics = m_markdownStyle->metrics();
    if (bodyPixelSize > 0)
        metrics.bodyPixelSize = bodyPixelSize;
    if (!monospaceFamily.isEmpty())
        metrics.monospaceFamily = monospaceFamily;
    m_markdownStyle->setMetrics(metrics);

    applyViewers();
}

void TextPreviewController::setDocumentStyle(
    bool light, const QColor& codeBackground, const QColor& mutedText, const QColor& rule)
{
    m_light = light;

    MarkdownStyle::Metrics metrics = m_markdownStyle->metrics();
    if (codeBackground.isValid())
        metrics.codeBackground = codeBackground;
    if (mutedText.isValid())
        metrics.mutedText = mutedText;
    if (rule.isValid())
        metrics.rule = rule;
    // setMetrics restyles whatever document is attached, so a page already open
    // follows the theme rather than waiting for the next file.
    m_markdownStyle->setMetrics(metrics);

    // And the same for a source file, which the highlighter re-lays itself.
    m_highlighter->setLightBackground(light);
}

void TextPreviewController::updateDisplayText()
{
    // Whether this window can be rendered as Markdown, decided before a
    // character of it reaches the view -- which is the only place it can be
    // decided. Behind `textFormat: MarkdownText` the TextArea owns the document,
    // so setMarkdown() runs inside the item on the thread that draws, and once it
    // has started nothing can cancel it or interrupt it. Whatever it costs, the
    // window pays it in one go.
    //
    // Per window and not per file, like the fold: a report whose tables are all
    // in the first window renders from the second one on.
    const bool asked = m_markdownMode != MarkdownMode::Unset;
    m_markdownTableRows
        = m_markdownFile && m_markdownMode != MarkdownMode::Source ? longestTableRun(m_text) : 0;
    // Only where nobody has answered. A reader who asked for the page gets it and
    // waits for it knowingly; that is what the choice in the strip is for.
    m_markdownDeclined = !asked && m_markdownTableRows > kMarkdownTableRows;
    m_markdown = m_markdownFile && m_markdownMode != MarkdownMode::Source && !m_markdownDeclined;

    m_displayText = isRenderedHtml() ? withoutExternalReferences(m_text) : m_text;

    // A window with no line break in it reaches the text engine as one block of
    // half a million characters, which is itemised and shaped whole on the GUI
    // thread -- and a single line has no partial layout to fall back on, so the
    // window stops answering. Nothing should be handed a block that size; break
    // the runs that are too long.
    //
    // Only where a line break is what makes a block, which is the plain text and
    // source case. Markdown and a rendered page are parsed into blocks by their
    // own markup, and a newline inside a paragraph is folded back into a space
    // by both -- so a fold there would change what is shown and fix nothing.
    // A Markdown file being shown as source is the plain text case again, and is
    // folded like any other: nothing is parsing it any more.
    m_longLinesFolded = !m_markdown && !isRenderedHtml() && hasOverlongLine(m_displayText, kFoldedLineChars);
    if (m_longLinesFolded) {
        m_displayText = withLongLinesFolded(m_displayText, kFoldedLineChars, &m_lineRows);
    } else if (isNumbered()) {
        m_lineRows = lineNumbersFor(m_displayText);
    } else {
        // Nothing to number: a paged file gets no gutter, and a rendered page has
        // no lines in the file's sense.
        m_lineRows.clear();
    }

    // Over the text as the reader sees it, folds and all -- and after the fold,
    // deliberately: a fold is a layout device and must not hide text from a
    // search, so the hits are found in the same string the view is handed.
    refreshFind();
}

void TextPreviewController::applyViewers()
{
    if (!m_document)
        return;

    // One or the other, never both. The highlighter reacts to every change by
    // laying an overlay over the text, and the Markdown styling reacts to every
    // change by rewriting the formats underneath it -- on one document they
    // would spend the afternoon answering each other.
    if (m_markdown) {
        m_highlighter->attachTo(nullptr);
        m_markdownStyle->attachTo(m_document);
        return;
    }

    m_markdownStyle->detach();

    // Not over a folded window, for two reasons rather than one. A fold cuts
    // strings and comments in half, and the highlighter carries nothing from one
    // block to the next except block-comment state, so it would colour the
    // second half of every cut token as something it is not. And colouring per
    // block is the other half of what makes a minified window slow: 512 kB of
    // JSON holds around twenty thousand quoted strings, each one a setFormat()
    // call, with a format kept per character while it runs.
    //
    // The language itself is unchanged, so paging on to a window that does have
    // lines in it gets its colour back.
    // A search still marks its hits here, which is why the term is set before
    // the return: the fold is a layout device, and a reader who cannot find a
    // word in a minified window has been told it is not there. Attached with no
    // language, so nothing is coloured and everything is findable.
    m_highlighter->setSearchTerm(m_findTerm);
    if (m_longLinesFolded) {
        if (m_findTerm.isEmpty()) {
            m_highlighter->attachTo(nullptr);
            return;
        }
        m_highlighter->attachTo(m_document);
        m_highlighter->setLanguage(QString());
        return;
    }

    m_highlighter->attachTo(m_document);
    m_highlighter->setLanguage(m_language);
}

// ---- finding a word in what is on screen -----------------------------------

void TextPreviewController::refreshFind()
{
    const QList<int> before = m_findHits;
    m_findHits.clear();
    if (!m_findTerm.isEmpty()) {
        int at = m_displayText.indexOf(m_findTerm, 0, Qt::CaseInsensitive);
        while (at >= 0) {
            m_findHits.append(at);
            // Overlapping hits are not wanted: "aa" in "aaaa" is two matches,
            // which is what a reader counts.
            at = m_displayText.indexOf(m_findTerm, at + m_findTerm.size(), Qt::CaseInsensitive);
        }
    }
    m_findAt = m_findHits.isEmpty() ? 0 : qBound(0, m_findAt, static_cast<int>(m_findHits.size()) - 1);
    if (before != m_findHits)
        emit findChanged();
}

void TextPreviewController::find(const QString& term)
{
    if (m_findTerm == term)
        return;
    m_findTerm = term;
    m_findAt = 0;
    // The marks are the document's own formats rather than an overlay, so
    // wrapping, folding and a theme change cannot leave them behind: whatever
    // re-runs the formats re-runs these too.
    if (m_highlighter)
        m_highlighter->setSearchTerm(m_findTerm);
    applyViewers();
    refreshFind();
    emit findChanged();
}

void TextPreviewController::findNext()
{
    if (m_findHits.isEmpty())
        return;
    // Wrapping, because a reader at the last match wants the first one and not a
    // key that does nothing.
    m_findAt = (m_findAt + 1) % m_findHits.size();
    emit findChanged();
}

void TextPreviewController::findPrevious()
{
    if (m_findHits.isEmpty())
        return;
    m_findAt = static_cast<int>((m_findAt + m_findHits.size() - 1) % m_findHits.size());
    emit findChanged();
}

void TextPreviewController::clearFind()
{
    if (m_findTerm.isEmpty() && m_findHits.isEmpty())
        return;
    m_findTerm.clear();
    m_findHits.clear();
    m_findAt = 0;
    if (m_highlighter)
        m_highlighter->setSearchTerm(QString());
    applyViewers();
    emit findChanged();
}

QVariantList TextPreviewController::lineNumbers() const
{
    if (!isNumbered())
        return {};
    QVariantList numbers;
    numbers.reserve(m_lineRows.size());
    for (int number : m_lineRows)
        numbers.append(number);
    return numbers;
}

int TextPreviewController::lineCount() const
{
    if (m_lineRows.isEmpty())
        return 0;
    // The last row that is a line of the file, which is not the last row when a
    // fold made rows after it.
    for (auto it = m_lineRows.crbegin(); it != m_lineRows.crend(); ++it) {
        if (*it > 0)
            return *it;
    }
    return 0;
}

int TextPreviewController::lineNumberDigits() const
{
    return QString::number(qMax(1, lineCount())).size();
}

int TextPreviewController::findPosition() const
{
    if (m_findHits.isEmpty())
        return -1;
    return m_findHits.at(qBound(0, m_findAt, static_cast<int>(m_findHits.size()) - 1));
}

QString TextPreviewController::findSummary() const
{
    if (m_findTerm.isEmpty())
        return {};

    // Which of the two truths this is. A file held whole is its own window, and
    // saying "in this window" there would be a qualifier about nothing; a file
    // being paged has more of itself elsewhere, and a count that did not say so
    // would be read as a count for the file.
    const QString scope = isPaged() ? QStringLiteral(" in this window") : QString();
    if (m_findHits.isEmpty())
        return QStringLiteral("no matches%1").arg(scope);
    return QStringLiteral("%1 of %2%3").arg(findIndex()).arg(findCount()).arg(scope);
}

void TextPreviewController::load(const FileEntry& entry)
{
    if (!m_services.isValid()) {
        setErrorText(QStringLiteral("Application services are not available"));
        return;
    }
    if (m_task)
        m_task->requestCancel();

    m_entry = entry;
    const QString suffix = entry.uri.suffix().toLower();

    // Markdown is rendered, not coloured, so the highlighter stays off for it.
    // Rendering is settled per window in updateDisplayText(); this is only what
    // the file is.
    m_markdownFile = TextPreviewProvider::isMarkdown(suffix);
    m_markdown = m_markdownFile && m_markdownMode != MarkdownMode::Source;
    m_markdownDeclined = false;
    m_markdownTableRows = 0;
    m_isHtml = TextPreviewProvider::isRenderable(suffix);
    m_language
        = m_markdownFile ? QString() : SourceHighlighter::languageFor(entry.name, entry.mimeType, suffix);
    m_highlighter->setLanguage(m_language);
    // Stepping from a Markdown file to a source file, or back, changes which of
    // the two viewers the document needs.
    applyViewers();

    m_fileSystem = m_services.vfs->resolve(entry.uri);
    if (!m_fileSystem) {
        setErrorText(QStringLiteral("No drive is mounted for this file"));
        return;
    }

    m_fileSize = entry.size;
    m_windowOffset = 0;
    m_windowBytes = 0;
    m_hasMore = false;
    emit windowChanged();

    readWindow(0);
}

void TextPreviewController::readWindow(qint64 offset)
{
    if (!m_fileSystem || !m_services.isValid())
        return;

    if (m_task)
        m_task->requestCancel();

    setErrorText({});
    setLoading(true);

    auto* task = new ReadRangeTask(m_fileSystem, m_entry.uri, offset, kWindowBytes);
    m_task = task;
    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        m_task.clear();
        setLoading(false);

        if (task->state() == Task::State::Failed) {
            setErrorText(task->error().message);
            return;
        }
        if (task->state() != Task::State::Succeeded)
            return;

        // Undecodable bytes become replacement characters. A preview of a
        // mostly-text file still beats refusing to show anything -- and a
        // window cut mid-character is expected when paging by bytes.
        QStringDecoder decoder(QStringDecoder::Utf8);
        m_text = decoder.decode(task->contents());
        updateDisplayText();
        // Whether this window is folded decides whether it is coloured, so the
        // highlighter is settled before the new text reaches the document rather
        // than after: attached, it would colour the window we just folded to
        // avoid the cost of colouring it.
        applyViewers();

        m_windowOffset = task->actualOffset();
        m_windowBytes = task->contents().size();
        m_hasMore = task->hasMore();
        if (task->fileSize() >= 0)
            m_fileSize = task->fileSize();

        emit textChanged();
        emit windowChanged();
    });

    m_services.tasks->submit(task);
}

void TextPreviewController::nextWindow()
{
    if (!m_hasMore)
        return;
    readWindow(m_windowOffset + m_windowBytes);
}

void TextPreviewController::previousWindow()
{
    if (m_windowOffset <= 0)
        return;
    readWindow(std::max<qint64>(0, m_windowOffset - kWindowBytes));
}

void TextPreviewController::firstWindow()
{
    if (m_windowOffset > 0)
        readWindow(0);
}

void TextPreviewController::lastWindow()
{
    if (m_fileSize <= kWindowBytes)
        return;
    readWindow(std::max<qint64>(0, m_fileSize - kWindowBytes));
}

void TextPreviewController::seekToFraction(double fraction)
{
    if (m_fileSize <= 0)
        return;
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    readWindow(static_cast<qint64>(clamped * static_cast<double>(m_fileSize)));
}

TextPreviewProvider::TextPreviewProvider(PluginServices services)
    : m_services(services)
{
}

bool TextPreviewProvider::isRenderable(const QString& suffix)
{
    const QString lower = suffix.toLower();
    return lower == QLatin1String("html") || lower == QLatin1String("htm") || lower == QLatin1String("xhtml");
}

bool TextPreviewProvider::isMarkdown(const QString& suffix)
{
    const QString lower = suffix.toLower();
    return lower == QLatin1String("md") || lower == QLatin1String("markdown")
        || lower == QLatin1String("mdown") || lower == QLatin1String("mkd");
}

QList<ViewerOption> TextPreviewProvider::options(const FileEntry& entry) const
{
    if (entry.isDir)
        return {};
    const QString suffix = entry.uri.suffix();
    const bool markdown = isMarkdown(suffix);
    if (!markdown && !isRenderable(suffix))
        return {};

    ViewerOption mode;
    mode.key = QStringLiteral("mode");
    mode.title = QStringLiteral("Show");
    mode.choices = { QStringLiteral("Source"), QStringLiteral("Rendered") };
    // Opposite defaults, from the same question about who is looking. A file
    // manager showing a `.html` should show what is in it, and somebody who
    // wants the page can say so once and be remembered. A Markdown file is the
    // other way round: it is written to be read as prose, and its source is the
    // unusual thing to want.
    //
    // Markdown has the choice at all so that a reader can ask for the page of a
    // report this viewer declined to render -- see kMarkdownTableRows.
    mode.defaultChoice = markdown ? QStringLiteral("Rendered") : QStringLiteral("Source");
    return { mode };
}

QStringList TextPreviewProvider::textSuffixes()
{
    // Anything the highlighter knows is by definition text, so the two lists
    // cannot drift apart. The extras are the formats worth showing that have
    // no syntax to colour.
    static const QStringList extras = { QStringLiteral("txt"), QStringLiteral("text"), QStringLiteral("log"),
        QStringLiteral("md"), QStringLiteral("markdown"), QStringLiteral("mdown"), QStringLiteral("mkd"),
        QStringLiteral("diff"), QStringLiteral("patch"), QStringLiteral("csv"), QStringLiteral("tsv"),
        QStringLiteral("srt"), QStringLiteral("vtt"), QStringLiteral("env"), QStringLiteral("lock"),
        QStringLiteral("gitignore"), QStringLiteral("dockerignore") };

    static const QStringList all = [] {
        QStringList out = extras;
        const QStringList known = SourceHighlighter::knownSuffixes();
        for (const QString& suffix : known) {
            if (!out.contains(suffix))
                out.append(suffix);
        }
        // A database and a Parquet file are binary. They have their own viewers,
        // and showing either as text would fill the window with replacement
        // characters -- worse than showing nothing.
        for (const QString& binary : SqlitePreviewProvider::databaseSuffixes())
            out.removeAll(binary);
        out.removeAll(QStringLiteral("parquet"));
        out.removeAll(QStringLiteral("pq"));
        return out;
    }();
    return all;
}

bool TextPreviewProvider::canPreview(const FileEntry& entry) const
{
    if (entry.isDir)
        return false;

    static const QMimeDatabase mimeDatabase;

    // Something has already looked inside this file, so what is in it decides and
    // the name does not get a second say: a Dockerfile is text however unknown its
    // name, and a zip called notes.txt is not text however familiar its name is.
    // Still no I/O here -- the answer arrived in the entry. See ADR-0033.
    if (!entry.mimeType.isEmpty())
        return mimeDatabase.mimeTypeForName(entry.mimeType).inherits(QStringLiteral("text/plain"));

    const QString suffix = entry.uri.suffix();
    if (textSuffixes().contains(suffix))
        return true;
    if (!suffix.isEmpty())
        return false;

    // No suffix: ask the shared MIME database rather than guessing. Name-only
    // lookup, because opening the file to sniff it would be I/O in canPreview.
    const QMimeType type = mimeDatabase.mimeTypeForFile(entry.name, QMimeDatabase::MatchExtension);
    return type.inherits(QStringLiteral("text/plain"));
}

QUrl TextPreviewProvider::viewSource() const
{
    return qmlView("TextPreview.qml");
}

PreviewController* TextPreviewProvider::createController(QObject* parent)
{
    return new TextPreviewController(m_services, parent);
}

// ------------------------------------------------------------------- hex

HexPreviewController::HexPreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
    , m_services(services)
{
}

HexPreviewController::~HexPreviewController()
{
    if (m_task)
        m_task->requestCancel();
}

int HexPreviewController::offsetDigits() const
{
    // Eight is right up to 4 GB and the width every hex dump has always been.
    // Past that the column widens rather than the offsets running into each
    // other, in pairs so the digits stay readable.
    int digits = 8;
    for (qint64 limit = 0xffffffffLL; m_fileSize > limit && digits < 16; limit <<= 8)
        digits += 2;
    return digits;
}

QString HexPreviewController::sizeText() const
{
    return m_fileSize >= 0 ? QLocale().formattedDataSize(m_fileSize) : QString();
}

QString HexPreviewController::positionText() const
{
    if (m_fileSize <= 0)
        return {};
    if (!isPaged())
        return sizeText();

    const QLocale locale;
    return QStringLiteral("%1 – %2 of %3")
        .arg(locale.formattedDataSize(m_windowOffset),
            locale.formattedDataSize(m_windowOffset + windowBytes()), locale.formattedDataSize(m_fileSize));
}

QString HexPreviewController::selectionSummary() const
{
    if (m_selectionLength <= 0)
        return {};
    const QLocale locale;
    return QStringLiteral("%1 bytes selected from %2")
        .arg(locale.toString(m_selectionLength))
        .arg(m_selectionStart, offsetDigits(), 16, QLatin1Char('0'));
}

void HexPreviewController::load(const FileEntry& entry)
{
    if (!m_services.isValid()) {
        setErrorText(QStringLiteral("Application services are not available"));
        return;
    }
    if (m_task)
        m_task->requestCancel();

    m_entry = entry;
    m_fileSystem = m_services.vfs->resolve(entry.uri);
    if (!m_fileSystem) {
        setErrorText(QStringLiteral("No drive is mounted for this file"));
        return;
    }

    m_fileSize = entry.size;
    m_windowOffset = 0;
    m_window.clear();
    clearSelection();
    rebuildRows();
    emit windowChanged();

    // An empty file is not read: there is nothing to read, and the view says so
    // rather than showing a grid with no rows in it.
    if (m_fileSize == 0)
        return;

    readWindow(0);
}

void HexPreviewController::readWindow(qint64 offset)
{
    if (!m_fileSystem || !m_services.isValid())
        return;

    if (m_task)
        m_task->requestCancel();

    setErrorText({});
    setLoading(true);

    // Snapped down to a row, so the offset column reads in sixteens whatever the
    // slider was dragged to, and never aligned to lines: these are bytes.
    const qint64 aligned = offset - offset % kBytesPerRow;
    auto* task = new ReadRangeTask(m_fileSystem, m_entry.uri, aligned, kWindowBytes);
    task->setAlignToLines(false);
    m_task = task;

    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        m_task.clear();
        setLoading(false);

        if (task->state() == Task::State::Failed) {
            // The grid is emptied as well as the error shown: rows from the last
            // window under a message about this one would be a lie about both.
            m_window.clear();
            m_rows.clear();
            clearSelection();
            setErrorText(task->error().message);
            emit windowChanged();
            return;
        }
        if (task->state() != Task::State::Succeeded)
            return;

        m_window = task->contents();
        m_windowOffset = task->actualOffset();
        m_hasMore = task->hasMore();
        if (task->fileSize() >= 0)
            m_fileSize = task->fileSize();

        // The selection belongs to the bytes on screen, so paging drops it.
        clearSelection();
        rebuildRows();
        emit windowChanged();
    });

    m_services.tasks->submit(task);
}

void HexPreviewController::rebuildRows()
{
    m_rows.clear();
    const int digits = offsetDigits();
    const qsizetype rowCount = (m_window.size() + kBytesPerRow - 1) / kBytesPerRow;
    m_rows.reserve(rowCount);

    for (qsizetype row = 0; row < rowCount; ++row) {
        const qsizetype start = row * kBytesPerRow;
        const qsizetype length = std::min<qsizetype>(kBytesPerRow, m_window.size() - start);

        QString hex;
        hex.reserve(kBytesPerRow * 3 + 1);
        QString text;
        text.reserve(kBytesPerRow);
        for (qsizetype i = 0; i < kBytesPerRow; ++i) {
            // A short final row is padded rather than ragged: the text column
            // has to stay where it is or the last line of every file jumps.
            if (i == kBytesPerRow / 2)
                hex += QLatin1Char(' ');
            if (i >= length) {
                hex += QStringLiteral("   ");
                continue;
            }
            const auto byte = static_cast<unsigned char>(m_window.at(start + i));
            hex += QStringLiteral("%1 ").arg(byte, 2, 16, QLatin1Char('0'));
            text += byte >= 0x20 && byte < 0x7f ? QChar::fromLatin1(static_cast<char>(byte))
                                                : QLatin1Char('.');
        }

        m_rows.append(QVariantMap {
            { QStringLiteral("offset"),
                QStringLiteral("%1").arg(m_windowOffset + start, digits, 16, QLatin1Char('0')) },
            { QStringLiteral("hex"), hex },
            { QStringLiteral("text"), text },
        });
    }
}

void HexPreviewController::nextWindow()
{
    if (!m_hasMore)
        return;
    readWindow(m_windowOffset + m_window.size());
}

void HexPreviewController::previousWindow()
{
    if (m_windowOffset <= 0)
        return;
    readWindow(std::max<qint64>(0, m_windowOffset - kWindowBytes));
}

void HexPreviewController::firstWindow()
{
    if (m_windowOffset > 0)
        readWindow(0);
}

void HexPreviewController::lastWindow()
{
    if (m_fileSize <= kWindowBytes)
        return;
    readWindow(std::max<qint64>(0, m_fileSize - kWindowBytes));
}

void HexPreviewController::seekToFraction(double fraction)
{
    if (m_fileSize <= 0)
        return;
    const double clamped = std::clamp(fraction, 0.0, 1.0);
    readWindow(static_cast<qint64>(clamped * static_cast<double>(m_fileSize)));
}

void HexPreviewController::selectRange(qint64 fromByte, qint64 toByte)
{
    if (m_window.isEmpty())
        return;

    const qint64 windowEnd = m_windowOffset + m_window.size() - 1;
    const qint64 first = std::clamp(std::min(fromByte, toByte), m_windowOffset, windowEnd);
    const qint64 last = std::clamp(std::max(fromByte, toByte), m_windowOffset, windowEnd);

    m_selectionStart = first;
    m_selectionLength = last - first + 1;
    emit selectionChanged();
}

void HexPreviewController::clearSelection()
{
    if (m_selectionLength == 0 && m_selectionStart < 0)
        return;
    m_selectionStart = -1;
    m_selectionLength = 0;
    emit selectionChanged();
}

QByteArray HexPreviewController::selectedBytes() const
{
    if (m_selectionLength <= 0)
        return {};
    return m_window.mid(
        static_cast<qsizetype>(m_selectionStart - m_windowOffset), static_cast<qsizetype>(m_selectionLength));
}

QString HexPreviewController::selectionAsHex() const
{
    const QByteArray bytes = selectedBytes();
    QStringList out;
    out.reserve(bytes.size());
    for (const char raw : bytes)
        out.append(QStringLiteral("%1").arg(static_cast<unsigned char>(raw), 2, 16, QLatin1Char('0')));
    return out.join(QLatin1Char(' '));
}

QString HexPreviewController::selectionAsText() const
{
    const QByteArray bytes = selectedBytes();
    QString out;
    out.reserve(bytes.size());
    for (const char raw : bytes) {
        const auto byte = static_cast<unsigned char>(raw);
        out += byte >= 0x20 && byte < 0x7f ? QChar::fromLatin1(raw) : QLatin1Char('.');
    }
    return out;
}

void HexPreviewController::copySelectionAsHex()
{
    const QString hex = selectionAsHex();
    if (!hex.isEmpty())
        QGuiApplication::clipboard()->setText(hex);
}

void HexPreviewController::copySelectionAsText()
{
    const QString text = selectionAsText();
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

HexPreviewProvider::HexPreviewProvider(PluginServices services)
    : m_services(services)
{
}

bool HexPreviewProvider::canPreview(const FileEntry& entry) const
{
    if (entry.isDir)
        return false;

    // Only what the content pass could make nothing of. application/octet-stream
    // is that answer: no magic rule matched, no glob matched, and it is not text.
    //
    // Claiming everything binary was this viewer's first rule and it was wrong:
    // a video, an mp3 and a .docx can all be named, and telling somebody what
    // their first 64 kB look like in hexadecimal is strictly less than telling
    // them how long the video runs. A file that can be named goes to the
    // information viewer, which has the facts -- and offers the bytes as a
    // choice, because they are occasionally what somebody came for. No I/O here
    // either: the type arrived in the entry. See ADR-0033.
    return entry.mimeType == QLatin1String("application/octet-stream");
}

QUrl HexPreviewProvider::viewSource() const
{
    return qmlView("HexPreview.qml");
}

PreviewController* HexPreviewProvider::createController(QObject* parent)
{
    return new HexPreviewController(m_services, parent);
}

// ----------------------------------------------------------------- image

ImagePreviewController::ImagePreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
    , m_copy(new LocalCopyProvider(services, this))
{
    connect(m_copy, &LocalCopyProvider::ready, this, [this](const QString& url) {
        setLoading(false);
        m_source = url;
        emit sourceChanged();
        // Before the view has a source, so the button is already right the first
        // time it is drawn rather than being corrected after a failed attempt.
        examineHeader(QUrl(url).toLocalFile());
    });
    connect(m_copy, &LocalCopyProvider::failed, this, [this](const QString& reason) {
        setLoading(false);
        setErrorText(reason);
    });
}

void ImagePreviewController::load(const FileEntry& entry)
{
    setErrorText({});
    setLoading(true);
    m_source.clear();
    emit sourceChanged();
    m_actualSizeAvailable = true;
    m_actualSizeReason.clear();
    emit actualSizeChanged();
    m_copy->request(entry.uri);
}

void ImagePreviewController::examineHeader(const QString& path)
{
    // The header and nothing else: QImageReader::size() is read out of it
    // without decoding a pixel, and allocationLimit() is what this build of Qt
    // will let a handler allocate. The file is already on local disk by the time
    // this runs, so it costs an open and a few dozen bytes.
    QImageReader reader(path);
    const QSize pixels = reader.size();
    const int limitMiB = QImageReader::allocationLimit();

    // A handler that will not say how large the image is, or a build with the
    // limit turned off, leaves 1:1 offered: nothing here says it cannot be done,
    // and reportDecodeFailure() is where an attempt that then fails is caught.
    if (!pixels.isValid() || limitMiB <= 0)
        return;

    // What the decode would ask for, in the format the handler says it would
    // produce. One that will not say is assumed to want the widest, because
    // guessing low here is what shows an empty frame.
    int bits = 32;
    const QImage::Format format = reader.imageFormat();
    if (format != QImage::Format_Invalid)
        bits = qMax(1, int(QImage::toPixelFormat(format).bitsPerPixel()));

    const qint64 bytes = qint64(pixels.width()) * pixels.height() * bits / 8;
    if (bytes <= qint64(limitMiB) * 1024 * 1024)
        return;

    // The size is in the reason on purpose. A greyed button with no explanation
    // reads as the application being broken; this reads as the picture being
    // bigger than the machine will hold at once.
    withdrawActualSize(
        QStringLiteral("too large to show at full size (%1 × %2)").arg(pixels.width()).arg(pixels.height()));
}

void ImagePreviewController::withdrawActualSize(const QString& reason)
{
    if (!m_actualSizeAvailable && m_actualSizeReason == reason)
        return;
    m_actualSizeAvailable = false;
    m_actualSizeReason = reason;
    emit actualSizeChanged();
}

void ImagePreviewController::reportDecodeFailure(bool atActualSize)
{
    // Only once there is something to have failed at. A QML Image reports Error
    // for an empty source as well, which is what it has between the file being
    // opened and the local copy arriving -- and a viewer that gave the file up in
    // that gap would never show an image at all.
    if (m_source.isEmpty())
        return;

    // The file is not what failed here: it was on screen, fitted, until somebody
    // asked to see it closer. The header does not always know -- a handler that
    // will not report a size leaves 1:1 offered -- so this is the other place
    // that finds out, and the answer is the same one: the button goes, with a
    // reason beside it, and the picture stays.
    if (atActualSize) {
        withdrawActualSize(QStringLiteral("too large to show at full size"));
        return;
    }

    decline(QStringLiteral("this build's image plugins could not decode it"));
}

ImagePreviewProvider::ImagePreviewProvider(PluginServices services)
    : m_services(services)
{
}

QStringList ImagePreviewProvider::imageSuffixes()
{
    // Asked of Qt rather than hard-coded: which formats exist depends on which
    // image plugins the build has, and claiming one we cannot decode would
    // show an empty frame instead of the file's details.
    QStringList suffixes;
    const QList<QByteArray> formats = QImageReader::supportedImageFormats();
    for (const QByteArray& format : formats)
        suffixes.append(QString::fromLatin1(format).toLower());
    return suffixes;
}

bool ImagePreviewProvider::canPreview(const FileEntry& entry) const
{
    if (entry.isDir)
        return false;
    static const QStringList supported = imageSuffixes();
    return supported.contains(entry.uri.suffix());
}

QUrl ImagePreviewProvider::viewSource() const
{
    return qmlView("ImagePreview.qml");
}

PreviewController* ImagePreviewProvider::createController(QObject* parent)
{
    return new ImagePreviewController(m_services, parent);
}

// ----------------------------------------------------------------- table

TablePreviewController::TablePreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
    , m_services(services)
    , m_table(new TableModel(this))
{
}

TablePreviewController::~TablePreviewController()
{
    if (m_task)
        m_task->requestCancel();
    // The model must let go before the store it reads through goes away.
    m_table->clear();
}

QStringList TablePreviewController::separatorChoices() const
{
    return { QStringLiteral(","), QStringLiteral("Tab"), QStringLiteral(";"), QStringLiteral("|") };
}

QString TablePreviewController::separator() const
{
    if (m_separator == QLatin1Char('\t'))
        return QStringLiteral("Tab");
    return m_separator.isNull() ? QStringLiteral(",") : QString(m_separator);
}

void TablePreviewController::setSeparator(const QString& separator)
{
    const QChar wanted = separator == QLatin1String("Tab") ? QLatin1Char('\t')
        : separator.isEmpty()                              ? QLatin1Char(',')
                                                           : separator.at(0);
    if (m_separator == wanted)
        return;
    m_separator = wanted;
    reimport();
}

void TablePreviewController::setFirstRowIsHeader(bool isHeader)
{
    if (m_firstRowIsHeader == isHeader)
        return;
    m_firstRowIsHeader = isHeader;
    reimport();
}

void TablePreviewController::copyBlock(int topRow, int leftColumn, int bottomRow, int rightColumn)
{
    const QString text = m_table->blockAsText(topRow, leftColumn, bottomRow, rightColumn);
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void TablePreviewController::updateSummary()
{
    const qint64 total = m_table->totalRows();
    const qint64 matching = m_table->matchingRows();

    const QLocale locale;
    QString text
        = QStringLiteral("%1 rows × %2 columns").arg(locale.toString(total)).arg(m_table->columnCount());
    if (matching != total)
        text += QStringLiteral("  ·  %1 match the filter").arg(locale.toString(matching));
    if (m_importing)
        text += QStringLiteral("  ·  still importing");

    m_summary = text;
    emit optionsChanged();
}

void TablePreviewController::load(const FileEntry& entry)
{
    if (!m_services.isValid()) {
        setErrorText(QStringLiteral("Application services are not available"));
        return;
    }

    m_entry = entry;
    // A .tsv is tab-separated by definition; anything else gets detected.
    m_separator = entry.uri.suffix() == QLatin1String("tsv") ? QLatin1Char('\t') : QChar();
    reimport();
}

void TablePreviewController::reimport()
{
    if (!m_services.isValid() || !m_entry.uri.isValid())
        return;

    if (m_task)
        m_task->requestCancel();

    FileSystemPtr fs = m_services.vfs->resolve(m_entry.uri);
    if (!fs) {
        setErrorText(QStringLiteral("No drive is mounted for this file"));
        return;
    }

    // A fresh database each time: re-importing with a different separator is
    // a different table, and reusing the old one would leave the previous
    // shape's columns behind.
    m_table->clear();
    m_store.reset();
    // Held by the store rather than by this controller: the store is what has to
    // outlive a reader moving on, and the file it writes to lives in here.
    QString why;
    std::shared_ptr<QTemporaryDir> scratch = staging::makeDirectory(&why);
    if (!scratch) {
        setErrorText(QStringLiteral("Could not make a scratch database: %1").arg(why));
        return;
    }

    m_store = std::make_shared<DelimitedStore>(
        QDir(scratch->path()).filePath(QStringLiteral("table.sqlite")), scratch);

    QString error;
    if (!m_store->open(&error)) {
        setErrorText(error);
        m_store.reset();
        return;
    }

    // Attached before the import starts, not after it finishes. The store is a
    // database that answers queries about whatever has been committed to it so
    // far, so the grid can show the first rows while the rest are still being
    // read -- and a model with no source reports no rows however many have
    // arrived, which is what made a large file look like a hang.
    m_table->setSource(m_store, m_services.tasks);

    setErrorText({});
    setLoading(true);
    m_importing = true;
    m_importedRows = 0;
    emit importProgress();

    auto* task = new ImportDelimitedTask(std::move(fs), m_entry.uri, m_store);
    task->setSeparator(m_separator);
    task->setFirstRowIsHeader(m_firstRowIsHeader);
    m_task = task;

    // The detected separator, as soon as it is known rather than at the end, so
    // the picker above a half-filled grid is telling the truth about it.
    connect(task, &ImportDelimitedTask::separatorDetected, this, [this, task](QChar separator) {
        if (m_task != task || m_separator == separator)
            return;
        m_separator = separator;
        emit optionsChanged();
    });

    // Rows appear as they arrive rather than after the whole file: on a large
    // export the first screen is usable long before the import finishes.
    connect(task, &ImportDelimitedTask::rowsImported, this, [this, task](qint64 rows) {
        if (m_task != task)
            return;
        m_importedRows = rows;
        m_table->refresh();
        updateSummary();
        emit importProgress();
    });

    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        m_task.clear();
        setLoading(false);
        m_importing = false;

        if (task->state() == Task::State::Failed) {
            setErrorText(task->error().message);
            emit importProgress();
            return;
        }
        if (task->state() != Task::State::Succeeded) {
            emit importProgress();
            return;
        }

        m_separator = task->separator();
        m_importedRows = task->importedRows();
        // Refreshed rather than re-sourced: the source has been attached since
        // before the import began, and setting it again would clear a filter
        // typed while the file was still being read.
        m_table->refresh();
        updateSummary();
        emit importProgress();
    });

    m_services.tasks->submit(task);
}

// ------------------------------------------------------- database and parquet

SqlitePreviewController::SqlitePreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
    , m_services(services)
    , m_table(new TableModel(this))
    , m_copy(std::make_unique<LocalCopyProvider>(services))
{
    // The database has to be a real file on disk for SQLite to open it, so a
    // copy is made first for anything on a remote or archive drive.
    connect(m_copy.get(), &LocalCopyProvider::ready, this, [this](const QString& fileUrl) {
        setLoading(false);

        m_table->clear();
        m_database = std::make_shared<SqliteTable>(QUrl(fileUrl).toLocalFile());

        QString error;
        if (!m_database->open(&error)) {
            m_database.reset();
            setErrorText(error);
            emit schemaChanged();
            return;
        }

        m_table->setSource(m_database, m_services.tasks);
        refreshSummary();
        countTables();
    });
    connect(m_copy.get(), &LocalCopyProvider::failed, this, [this](const QString& reason) {
        setLoading(false);
        setErrorText(reason);
    });
}

SqlitePreviewController::~SqlitePreviewController()
{
    // Nothing is waiting for the counts any more, and a database of large
    // tables would go on being walked for as long as it took.
    if (m_counting)
        m_counting->requestCancel();
    // The model must let go before the database it reads through goes away.
    m_table->clear();
}

void SqlitePreviewController::countTables()
{
    if (!m_database)
        return;

    // The table being looked at first, then the rest. Every count is wanted --
    // the picker shows one per name -- but only one of them is the number under
    // the grid somebody is already reading.
    QStringList order = m_database->tableNames();
    const QString current = m_database->currentTable();
    if (order.removeAll(current) > 0)
        order.prepend(current);

    auto* task = new CountTableRowsTask(m_database->path(), order);
    m_counting = task;
    connect(task, &CountTableRowsTask::counted, this, [this](const QString& table, qint64 rows) {
        if (!m_database)
            return;
        m_database->setRowCount(table, rows);
        // The count of the table on screen is the row count of the grid, so the
        // model has to be told; the others are only text in the picker.
        if (table == m_database->currentTable())
            m_table->refresh();
        refreshSummary();
    });
    m_services.tasks->submit(task);
}

void SqlitePreviewController::refreshSummary()
{
    if (!m_database) {
        m_summary.clear();
    } else {
        const QLocale locale;
        const qint64 total = m_table->totalRows();
        // Without the total until the count arrives, rather than with a nought
        // that would read as an empty table or a guess that would be wrong.
        m_summary = total < 0
            ? QStringLiteral("%1 · %2 columns").arg(m_database->currentTable()).arg(m_table->columnCount())
            : QStringLiteral("%1 · %2 rows × %3 columns")
                  .arg(m_database->currentTable(), locale.toString(total))
                  .arg(m_table->columnCount());
        if (m_table->matchingRows() >= 0 && m_table->matchingRows() != total) {
            m_summary
                += QStringLiteral("  ·  %1 match the filter").arg(locale.toString(m_table->matchingRows()));
        }
    }
    emit schemaChanged();
}

QVariantList SqlitePreviewController::tables() const
{
    QVariantList out;
    if (!m_database)
        return out;

    const QLocale locale;
    const QStringList names = m_database->tableNames();
    for (const QString& name : names) {
        // The count as it stands, never a count taken here: this is read by a
        // binding, on the thread that draws, and once per name. Blank until the
        // counting pass reaches this table.
        const qint64 rows = m_database->knownRowCountOf(name);
        out.append(QVariantMap { { QStringLiteral("name"), name },
            { QStringLiteral("rowsText"), rows < 0 ? QString() : locale.toString(rows) },
            { QStringLiteral("current"), name == m_database->currentTable() } });
    }
    return out;
}

QString SqlitePreviewController::currentTable() const
{
    return m_database ? m_database->currentTable() : QString();
}

void SqlitePreviewController::setCurrentTable(const QString& table)
{
    if (!m_database || table.isEmpty() || table == m_database->currentTable())
        return;
    if (!m_database->setCurrentTable(table))
        return;
    // Its count may not have been taken yet, in which case the grid fills in
    // when it arrives -- the same way the first table did.

    // A different table is a different shape, so the model is repointed rather
    // than refreshed -- its cached pages and column widths belong to the old one.
    m_table->setSource(m_database, m_services.tasks);
    refreshSummary();
}

void SqlitePreviewController::copyBlock(int topRow, int leftColumn, int bottomRow, int rightColumn)
{
    const QString text = m_table->blockAsText(topRow, leftColumn, bottomRow, rightColumn);
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void SqlitePreviewController::load(const FileEntry& entry)
{
    setErrorText({});
    setLoading(true);
    // Counts belong to the file that was open when they were asked for.
    if (m_counting)
        m_counting->requestCancel();
    m_table->clear();
    m_database.reset();
    emit schemaChanged();
    m_copy->request(entry.uri);
}

SqlitePreviewProvider::SqlitePreviewProvider(PluginServices services)
    : m_services(services)
{
}

QStringList SqlitePreviewProvider::databaseSuffixes()
{
    // The conventional ones. A SQLite file has a magic header rather than a
    // required extension, but sniffing it would mean reading in canPreview.
    return { QStringLiteral("sqlite"), QStringLiteral("sqlite3"), QStringLiteral("db"), QStringLiteral("db3"),
        QStringLiteral("s3db"), QStringLiteral("sl3") };
}

bool SqlitePreviewProvider::canPreview(const FileEntry& entry) const
{
    return !entry.isDir && databaseSuffixes().contains(entry.uri.suffix().toLower());
}

QUrl SqlitePreviewProvider::viewSource() const
{
    return qmlView("SqlitePreview.qml");
}

PreviewController* SqlitePreviewProvider::createController(QObject* parent)
{
    return new SqlitePreviewController(m_services, parent);
}

ParquetPreviewController::ParquetPreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
    , m_services(services)
    , m_table(new TableModel(this))
    , m_copy(std::make_unique<LocalCopyProvider>(services))
{
    connect(m_copy.get(), &LocalCopyProvider::ready, this, [this](const QString& fileUrl) {
        setLoading(false);

        m_table->clear();
        m_file = std::make_shared<ParquetTable>(QUrl(fileUrl).toLocalFile());

        QString error;
        if (!m_file->open(&error)) {
            m_file.reset();
            setErrorText(error);
            emit schemaChanged();
            return;
        }

        m_table->setSource(m_file, m_services.tasks);

        const QLocale locale;
        m_summary = QStringLiteral("%1 rows × %2 columns · %3")
                        .arg(locale.toString(m_table->totalRows()))
                        .arg(m_table->columnCount())
                        .arg(m_file->fileSummary());
        emit schemaChanged();
    });
    connect(m_copy.get(), &LocalCopyProvider::failed, this, [this](const QString& reason) {
        setLoading(false);
        setErrorText(reason);
    });
}

ParquetPreviewController::~ParquetPreviewController()
{
    m_table->clear();
}

QStringList ParquetPreviewController::columnTypes() const
{
    return m_file ? m_file->columnTypes() : QStringList {};
}

void ParquetPreviewController::copyBlock(int topRow, int leftColumn, int bottomRow, int rightColumn)
{
    const QString text = m_table->blockAsText(topRow, leftColumn, bottomRow, rightColumn);
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void ParquetPreviewController::load(const FileEntry& entry)
{
    setErrorText({});
    setLoading(true);
    m_table->clear();
    m_file.reset();
    m_summary.clear();
    emit schemaChanged();
    m_copy->request(entry.uri);
}

ParquetPreviewProvider::ParquetPreviewProvider(PluginServices services)
    : m_services(services)
{
}

bool ParquetPreviewProvider::canPreview(const FileEntry& entry) const
{
    // False without Arrow, so the file falls through to the information viewer
    // rather than opening a grid that can never fill.
    if (!ParquetTable::isSupported() || entry.isDir)
        return false;
    const QString suffix = entry.uri.suffix().toLower();
    return suffix == QLatin1String("parquet") || suffix == QLatin1String("pq");
}

QUrl ParquetPreviewProvider::viewSource() const
{
    return qmlView("ParquetPreview.qml");
}

PreviewController* ParquetPreviewProvider::createController(QObject* parent)
{
    return new ParquetPreviewController(m_services, parent);
}

TablePreviewProvider::TablePreviewProvider(PluginServices services)
    : m_services(services)
{
}

bool TablePreviewProvider::canPreview(const FileEntry& entry) const
{
    if (entry.isDir)
        return false;
    const QString suffix = entry.uri.suffix();
    return suffix == QLatin1String("csv") || suffix == QLatin1String("tsv");
}

QUrl TablePreviewProvider::viewSource() const
{
    return qmlView("TablePreview.qml");
}

PreviewController* TablePreviewProvider::createController(QObject* parent)
{
    return new TablePreviewController(m_services, parent);
}

// ------------------------------------------------------------- file info

// -------------------------------------------------------------- json lines

JsonLinesPreviewController::JsonLinesPreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
    , m_services(services)
    , m_table(new TableModel(this))
{
}

JsonLinesPreviewController::~JsonLinesPreviewController()
{
    if (m_task)
        m_task->requestCancel();
    // The model must let go before the store it reads through goes away.
    m_table->clear();
}

QObject* JsonLinesPreviewController::source() const
{
    return m_text.data();
}

QString JsonLinesPreviewController::sourceReason() const
{
    // Only when the file decided it. A reader who chose the source knows why the
    // source is on screen, and a sentence explaining it would be noise.
    if (!m_notRecords)
        return {};
    return QStringLiteral("shown as source: the records here are not JSON objects");
}

void JsonLinesPreviewController::copyBlock(int topRow, int leftColumn, int bottomRow, int rightColumn)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard())
        clipboard->setText(m_table->blockAsText(topRow, leftColumn, bottomRow, rightColumn));
}

void JsonLinesPreviewController::load(const FileEntry& entry)
{
    if (!m_services.isValid()) {
        setErrorText(QStringLiteral("Application services are not available"));
        return;
    }

    m_entry = entry;
    // Per file: the last file being the wrong shape says nothing about this one.
    m_notRecords = false;
    m_records = 0;
    m_skippedLines = 0;
    m_keysWithoutAColumn = 0;

    if (m_showSource) {
        // Asked for before the file was read, so there is nothing to import. The
        // grid is built if the reader changes their mind, and not before.
        showSource();
        updateSummary();
        return;
    }
    import();
}

void JsonLinesPreviewController::setViewerOption(const QString& key, const QString& value)
{
    if (key != QLatin1String("mode"))
        return;

    const bool wantsSource = value.compare(QLatin1String("Source"), Qt::CaseInsensitive) == 0;
    if (wantsSource == m_showSource)
        return;
    m_showSource = wantsSource;

    // Arrives before load() when it was remembered, and from the strip when it is
    // chosen, so both come through here -- and before load() there is no file yet,
    // which is why this only ever acts on one that is already open.
    if (m_entry.uri.isValid()) {
        if (m_showSource)
            showSource();
        else if (!m_store)
            import();
    }
    updateSummary();
}

void JsonLinesPreviewController::showSource()
{
    if (!m_text)
        m_text = new TextPreviewController(m_services, this);
    if (m_entry.uri.isValid())
        m_text->load(m_entry);
    emit importProgress();
}

void JsonLinesPreviewController::import()
{
    if (!m_services.isValid() || !m_entry.uri.isValid())
        return;

    if (m_task)
        m_task->requestCancel();

    FileSystemPtr fs = m_services.vfs->resolve(m_entry.uri);
    if (!fs) {
        setErrorText(QStringLiteral("No drive is mounted for this file"));
        return;
    }

    // A fresh database per file, as the delimited viewer does: the columns are
    // this file's keys, and reusing the last one would leave its shape behind.
    m_table->clear();
    m_store.reset();
    // Held by the store rather than by this controller: the store is what has to
    // outlive a reader moving on, and the file it writes to lives in here.
    QString why;
    std::shared_ptr<QTemporaryDir> scratch = staging::makeDirectory(&why);
    if (!scratch) {
        setErrorText(QStringLiteral("Could not make a scratch database: %1").arg(why));
        return;
    }

    m_store = std::make_shared<DelimitedStore>(
        QDir(scratch->path()).filePath(QStringLiteral("table.sqlite")), scratch);

    QString error;
    if (!m_store->open(&error)) {
        setErrorText(error);
        m_store.reset();
        return;
    }

    // Attached before the import starts, so the grid shows the first records
    // while the rest are still being read -- a model with no source reports no
    // rows however many have arrived.
    m_table->setSource(m_store, m_services.tasks);

    setErrorText({});
    setLoading(true);
    m_importing = true;
    m_records = 0;
    emit importProgress();

    auto* task = new ImportJsonLinesTask(std::move(fs), m_entry.uri, m_store);
    m_task = task;

    connect(task, &ImportJsonLinesTask::shapeSettled, this, [this, task] {
        if (m_task != task)
            return;
        // The columns are known now, and the caption above a filling grid should
        // say the real ones rather than nothing.
        m_table->refresh();
        updateSummary();
    });

    connect(task, &ImportJsonLinesTask::rowsImported, this, [this, task](qint64 rows) {
        if (m_task != task)
            return;
        m_records = rows;
        m_table->refresh();
        updateSummary();
        emit importProgress();
    });

    connect(task, &Task::finished, this, [this, task] {
        if (m_task != task)
            return;
        m_task.clear();
        setLoading(false);
        m_importing = false;

        if (task->state() == Task::State::Failed) {
            setErrorText(task->error().message);
            emit importProgress();
            return;
        }
        if (task->state() != Task::State::Succeeded) {
            emit importProgress();
            return;
        }

        m_records = task->importedRows();
        m_skippedLines = task->skippedLines();
        m_keysWithoutAColumn = task->keysWithoutAColumn();

        // A file that is not a list of records has no table to show, whatever the
        // preference says -- and the preference is not touched, because the reader
        // did not ask for this.
        m_notRecords = !task->looksLikeRecords();
        if (m_notRecords)
            showSource();

        m_table->refresh();
        updateSummary();
        emit importProgress();
    });

    m_services.tasks->submit(task);
}

void JsonLinesPreviewController::updateSummary()
{
    if (isShowingSource()) {
        m_summary = sourceReason();
        emit importProgress();
        return;
    }

    const qint64 total = m_table->totalRows();
    const qint64 matching = m_table->matchingRows();

    const QLocale locale;
    QString text
        = QStringLiteral("%1 records × %2 columns").arg(locale.toString(total)).arg(m_table->columnCount());
    if (matching != total)
        text += QStringLiteral("  ·  %1 match the filter").arg(locale.toString(matching));
    // Both counts are told rather than left to be noticed: a value that is not in
    // the grid, and a line that is not a row, are things a reader would otherwise
    // only find by comparing the file with the screen.
    if (m_keysWithoutAColumn > 0) {
        text += QStringLiteral("  ·  %1 value(s) under keys the head of the file did not have")
                    .arg(locale.toString(m_keysWithoutAColumn));
    }
    if (m_skippedLines > 0)
        text += QStringLiteral("  ·  %1 line(s) skipped").arg(locale.toString(m_skippedLines));
    if (m_importing)
        text += QStringLiteral("  ·  still reading");

    m_summary = text;
    emit importProgress();
}

JsonLinesPreviewProvider::JsonLinesPreviewProvider(PluginServices services)
    : m_services(services)
{
}

bool JsonLinesPreviewProvider::isJsonLines(const QString& suffix)
{
    const QString lower = suffix.toLower();
    return lower == QLatin1String("jsonl") || lower == QLatin1String("ndjson");
}

bool JsonLinesPreviewProvider::canPreview(const FileEntry& entry) const
{
    return !entry.isDir && isJsonLines(entry.uri.suffix());
}

QList<ViewerOption> JsonLinesPreviewProvider::options(const FileEntry& entry) const
{
    if (entry.isDir)
        return {};

    ViewerOption mode;
    mode.key = QStringLiteral("mode");
    mode.title = QStringLiteral("Show");
    mode.choices = { QStringLiteral("Table"), QStringLiteral("Source") };
    // The table, because that is what the file is: a list of records with the
    // same keys is a table written down one line at a time. Somebody who wants
    // the lines can say so once and be remembered.
    mode.defaultChoice = QStringLiteral("Table");
    return { mode };
}

QUrl JsonLinesPreviewProvider::viewSource() const
{
    return qmlView("JsonLinesPreview.qml");
}

PreviewController* JsonLinesPreviewProvider::createController(QObject* parent)
{
    return new JsonLinesPreviewController(m_services, parent);
}

FileInfoPreviewController::FileInfoPreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
    , m_services(services)
{
}

QObject* FileInfoPreviewController::bytes() const
{
    return m_hex.data();
}

void FileInfoPreviewController::load(const FileEntry& entry)
{
    // The name, and nothing else. What is known about the file is the generic
    // metadata reader's answer now, shown in the details panel this viewer opens
    // by default -- built once, for every viewer, rather than here for the one
    // case where no viewer claimed the file. See ADR-0034.
    m_entry = entry;
    m_headline = entry.name;
    if (m_showBytes && m_hex)
        m_hex->load(entry);
    emit factsChanged();
}

void FileInfoPreviewController::setViewerOption(const QString& key, const QString& value)
{
    if (key != QLatin1String("mode"))
        return;

    const bool bytes = value.compare(QLatin1String("Bytes"), Qt::CaseInsensitive) == 0;
    if (bytes == m_showBytes)
        return;
    m_showBytes = bytes;

    // Built the first time it is asked for, and never before: a choice nobody
    // made must not cost a read. Applied before load() when it was remembered,
    // and from the strip when it is chosen, so both arrive here.
    if (m_showBytes) {
        if (!m_hex)
            m_hex = new HexPreviewController(m_services, this);
        if (m_entry.uri.isValid())
            m_hex->load(m_entry);
    }
    emit factsChanged();
}

FileInfoPreviewProvider::FileInfoPreviewProvider(PluginServices services)
    : m_services(services)
{
}

QList<ViewerOption> FileInfoPreviewProvider::options(const FileEntry& entry) const
{
    if (entry.isDir)
        return {};

    ViewerOption mode;
    mode.key = QStringLiteral("mode");
    mode.title = QStringLiteral("Show");
    mode.choices = { QStringLiteral("Information"), QStringLiteral("Bytes") };
    // Information by default: what a file is answers more questions than what
    // its first page looks like, and the other one is one click away.
    mode.defaultChoice = QStringLiteral("Information");
    return { mode };
}

QUrl FileInfoPreviewProvider::viewSource() const
{
    return qmlView("FileInfoPreview.qml");
}

PreviewController* FileInfoPreviewProvider::createController(QObject* parent)
{
    return new FileInfoPreviewController(m_services, parent);
}

} // namespace mole
