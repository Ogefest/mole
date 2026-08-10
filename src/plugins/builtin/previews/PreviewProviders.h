#pragma once

#include "plugins/builtin/previews/MarkdownStyle.h"
#include "plugins/builtin/previews/SyntaxHighlighter.h"
#include "sdk/IPreviewProvider.h"
#include "ui/models/TableModel.h"

#include "core/data/ParquetTable.h"
#include "core/data/SqliteTable.h"
#include "core/tasks/ReadFileTask.h"
#include "core/tasks/ReadRangeTask.h"
#include "core/text/DelimitedStore.h"
#include "core/text/ImportDelimitedTask.h"

#include <QPointer>
#include <QTemporaryDir>

#include <memory>

class QQuickTextDocument;

namespace mole {

/// Turns a uri into something the desktop can point at.
///
/// Local files are used where they are; anything on a remote or archive drive
/// is streamed into a scratch directory first, because an `<Image>` element
/// cannot open `archive://`.
class LocalCopyProvider : public QObject
{
    Q_OBJECT

public:
    explicit LocalCopyProvider(PluginServices services, QObject* parent = nullptr);
    ~LocalCopyProvider() override;

    /// Emits ready() with a `file:` url, or failed() with a reason.
    void request(const VfsUri& uri, qint64 maxBytes = -1);
    void cancel();

signals:
    void ready(const QString& fileUrl);
    void failed(const QString& reason);

private:
    PluginServices m_services;
    std::unique_ptr<QTemporaryDir> m_scratch;
    QPointer<ReadFileTask> m_task;
};

// ---------------------------------------------------------------- text

/// Text and source code, coloured, and Markdown rendered.
///
/// The file is never held whole. Only the window being shown is read, through
/// a seek, so a 100 GB log opens as fast as a 100 byte one and paging through
/// it costs a chunk at a time rather than the whole file.
class TextPreviewController final : public PreviewController
{
    Q_OBJECT
    Q_PROPERTY(QString text READ text NOTIFY textChanged)
    Q_PROPERTY(bool highlighted READ isHighlighted NOTIFY textChanged)
    /// True when this window had runs too long to lay out and they were broken
    /// up to be shown. The view has to say so: a reader must not take a fold for
    /// a line break that is in the file.
    Q_PROPERTY(bool longLinesFolded READ longLinesFolded NOTIFY textChanged)
    /// Rendered rather than coloured. Markdown is meant to be read, not read
    /// as source -- if someone wants the source they can open it as text.
    Q_PROPERTY(bool markdown READ isMarkdown NOTIFY textChanged)
    Q_PROPERTY(QString languageName READ languageName NOTIFY textChanged)
    /// Rendered as a page rather than shown as source. Only ever true for markup
    /// the viewer was asked to render -- see setViewerOption().
    Q_PROPERTY(bool renderedHtml READ isRenderedHtml NOTIFY textChanged)

    /// Where this window sits in the file, for the position bar.
    Q_PROPERTY(qint64 fileSize READ fileSize NOTIFY windowChanged)
    Q_PROPERTY(qint64 windowOffset READ windowOffset NOTIFY windowChanged)
    Q_PROPERTY(qint64 windowBytes READ windowBytes NOTIFY windowChanged)
    /// True when the file is bigger than one window, so the interface knows
    /// whether to offer paging at all.
    Q_PROPERTY(bool paged READ isPaged NOTIFY windowChanged)
    Q_PROPERTY(bool atStart READ isAtStart NOTIFY windowChanged)
    Q_PROPERTY(bool atEnd READ isAtEnd NOTIFY windowChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY windowChanged)
    Q_PROPERTY(QString sizeText READ sizeText NOTIFY windowChanged)

public:
    explicit TextPreviewController(PluginServices services, QObject* parent = nullptr);
    ~TextPreviewController() override;

    /// The longest run the text engine is ever handed as one block. A line
    /// wider than this is not a line anybody wrote -- it is a minified file, a
    /// one-line dump or a base64 blob -- and a single block that long is
    /// itemised and shaped whole, on the GUI thread, with no partial layout to
    /// fall back on. Wide enough to leave every real line alone; narrow enough
    /// that a window of them lays out a block at a time.
    static constexpr qsizetype kFoldedLineChars = 4096;

    /// What the view shows: the source as read, or the page with everything that
    /// could reach off the disk taken out of it.
    QString text() const { return m_displayText; }
    /// False for a folded window even when the file has a language: see
    /// updateDisplayText().
    bool isHighlighted() const { return !m_language.isEmpty() && !m_longLinesFolded; }
    bool longLinesFolded() const { return m_longLinesFolded; }
    bool isMarkdown() const { return m_markdown; }
    bool isRenderedHtml() const { return m_renderHtml && m_isHtml; }
    QString languageName() const;

    qint64 fileSize() const { return m_fileSize; }
    qint64 windowOffset() const { return m_windowOffset; }
    qint64 windowBytes() const { return m_windowBytes; }
    bool isPaged() const { return m_fileSize > kWindowBytes; }
    bool isAtStart() const { return m_windowOffset <= 0; }
    bool isAtEnd() const { return !m_hasMore; }
    QString positionText() const;
    QString sizeText() const;

    void load(const FileEntry& entry) override;
    void setViewerOption(const QString& key, const QString& value) override;

    /// Removes everything a document could use to reach off the disk: images,
    /// scripts, stylesheets, frames, embedded objects and event handlers. Static
    /// and exposed because it is the rule that matters most here and deserves a
    /// test of its own -- previewing a file must put nothing on the network.
    static QString withoutExternalReferences(const QString& html);

    // ---- paging ---------------------------------------------------------

    Q_INVOKABLE void nextWindow();
    Q_INVOKABLE void previousWindow();
    Q_INVOKABLE void firstWindow();
    Q_INVOKABLE void lastWindow();
    /// Jumps to a point in the file, 0.0 .. 1.0. The window snaps to a line.
    Q_INVOKABLE void seekToFraction(double fraction);

    /// Called by QML with a TextArea's textDocument. Both colouring and Markdown
    /// typography have to attach to the real document; there is no way to do
    /// either from QML alone. The view also passes the two things it owns: the
    /// size it sets prose at, and the family it uses for code.
    Q_INVOKABLE void attachDocument(
        QQuickTextDocument* document, int bodyPixelSize, const QString& monospaceFamily);

signals:
    void textChanged();
    void windowChanged();

private:
    void readWindow(qint64 offset);
    /// Points whichever of the two the current file needs at the document, and
    /// takes the other one off it.
    void applyViewers();
    /// Recomputes what the view shows from what was read.
    void updateDisplayText();

    /// 512 kB is roughly ten thousand lines of code -- more than anyone reads
    /// in one screen, and small enough that paging feels instant.
    static constexpr qint64 kWindowBytes = 512 * 1024;

    PluginServices m_services;
    SourceHighlighter* m_highlighter = nullptr;
    MarkdownStyle* m_markdownStyle = nullptr;
    QPointer<QQuickTextDocument> m_document;
    QString m_language;
    bool m_markdown = false;
    bool m_isHtml = false;
    bool m_renderHtml = false;
    QString m_text;
    /// Derived from m_text and the chosen mode, computed once per change rather
    /// than on every read.
    QString m_displayText;
    /// Whether deriving it had to break long runs up. A property of the window
    /// rather than of the file: paging on to one with lines in it clears it.
    bool m_longLinesFolded = false;

    FileEntry m_entry;
    FileSystemPtr m_fileSystem;
    qint64 m_fileSize = -1;
    qint64 m_windowOffset = 0;
    qint64 m_windowBytes = 0;
    bool m_hasMore = false;

    QPointer<ReadRangeTask> m_task;
};

class TextPreviewProvider final : public IPreviewProvider
{
public:
    explicit TextPreviewProvider(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview.text"); }
    QString displayName() const override { return QStringLiteral("Text"); }
    int priority() const override { return -100; } ///< the fallback for text
    bool canPreview(const FileEntry& entry) const override;
    QUrl viewSource() const override;
    PreviewController* createController(QObject* parent) override;

    /// HTML can be read as source or shown as a page, and which one is right
    /// depends entirely on who is looking. Nothing else here has a choice to make.
    QList<ViewerOption> options(const FileEntry& entry) const override;

    static QStringList textSuffixes();
    /// Markup this viewer can render as a page rather than colour as source.
    static bool isRenderable(const QString& suffix);

private:
    PluginServices m_services;
};

// ----------------------------------------------------------------- hex

/// The bytes of a file, when nothing can say more than that.
///
/// **Read-only, and it will stay that way.** This is the viewer where somebody
/// would most expect to be able to type, which is exactly why the rule is worth
/// writing down here: previewing a file is not a licence to modify it, and there
/// is no code path in Mole that writes through a preview. An editor is a
/// different thing, wanted or not, and it would need undo, a save that cannot
/// half-happen, and a story for a file that changed underneath it.
///
/// Windowed like the text viewer, and for the same reason: the file is never
/// held, only the window being shown, so a 100 GB firmware image opens as fast
/// as a 100 byte one.
class HexPreviewController final : public PreviewController
{
    Q_OBJECT
    /// One entry per row of sixteen bytes: `offset`, `hex` and `text`. The
    /// absolute offset of a row's first byte is `windowOffset + index * 16`,
    /// which is what the view uses to place a selection.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY windowChanged)
    /// Said in words rather than shown as an empty grid.
    Q_PROPERTY(bool emptyFile READ isEmptyFile NOTIFY windowChanged)
    /// Hex digits in the offset column: eight, or more for a file past 4 GB.
    Q_PROPERTY(int offsetDigits READ offsetDigits NOTIFY windowChanged)

    /// Where this window sits in the file, for the position bar. The same set
    /// the text viewer exposes, because the strip and the keys are the same.
    Q_PROPERTY(qint64 fileSize READ fileSize NOTIFY windowChanged)
    Q_PROPERTY(qint64 windowOffset READ windowOffset NOTIFY windowChanged)
    Q_PROPERTY(qint64 windowBytes READ windowBytes NOTIFY windowChanged)
    Q_PROPERTY(bool paged READ isPaged NOTIFY windowChanged)
    Q_PROPERTY(bool atStart READ isAtStart NOTIFY windowChanged)
    Q_PROPERTY(bool atEnd READ isAtEnd NOTIFY windowChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY windowChanged)
    Q_PROPERTY(QString sizeText READ sizeText NOTIFY windowChanged)

    /// The selected run of bytes, absolute. -1 and 0 when there is no selection.
    Q_PROPERTY(qint64 selectionStart READ selectionStart NOTIFY selectionChanged)
    Q_PROPERTY(qint64 selectionLength READ selectionLength NOTIFY selectionChanged)
    /// "16 bytes selected", or empty. The strip says it because a byte range is
    /// not something the eye counts.
    Q_PROPERTY(QString selectionSummary READ selectionSummary NOTIFY selectionChanged)

public:
    explicit HexPreviewController(PluginServices services, QObject* parent = nullptr);
    ~HexPreviewController() override;

    /// 64 kB is 4096 rows -- more than anybody reads at once, and small enough
    /// that paging feels instant.
    static constexpr qint64 kWindowBytes = 64 * 1024;
    static constexpr int kBytesPerRow = 16;

    QVariantList rows() const { return m_rows; }
    bool isEmptyFile() const { return m_fileSize == 0; }
    int offsetDigits() const;

    qint64 fileSize() const { return m_fileSize; }
    qint64 windowOffset() const { return m_windowOffset; }
    qint64 windowBytes() const { return m_window.size(); }
    bool isPaged() const { return m_fileSize > kWindowBytes; }
    bool isAtStart() const { return m_windowOffset <= 0; }
    bool isAtEnd() const { return !m_hasMore; }
    QString positionText() const;
    QString sizeText() const;

    void load(const FileEntry& entry) override;

    // ---- paging ---------------------------------------------------------

    Q_INVOKABLE void nextWindow();
    Q_INVOKABLE void previousWindow();
    Q_INVOKABLE void firstWindow();
    Q_INVOKABLE void lastWindow();
    /// Jumps to a point in the file, 0.0 .. 1.0, snapped down to a row.
    Q_INVOKABLE void seekToFraction(double fraction);

    // ---- selecting and copying -------------------------------------------

    qint64 selectionStart() const { return m_selectionLength > 0 ? m_selectionStart : -1; }
    qint64 selectionLength() const { return m_selectionLength; }
    QString selectionSummary() const;

    /// Selects every byte between two absolute offsets, either way round, and
    /// clamped to the window on screen -- the bytes that can be selected are the
    /// bytes in front of the reader.
    Q_INVOKABLE void selectRange(qint64 fromByte, qint64 toByte);
    Q_INVOKABLE void clearSelection();

    /// The selection as `48 65 6c`, and as text with a dot for every byte that
    /// has no printable form. Both are wanted: a header is read as hex and a
    /// string inside a binary is read as text, and a selection that can only
    /// leave one way is half a viewer.
    Q_INVOKABLE QString selectionAsHex() const;
    Q_INVOKABLE QString selectionAsText() const;
    Q_INVOKABLE void copySelectionAsHex();
    Q_INVOKABLE void copySelectionAsText();

signals:
    void windowChanged();
    void selectionChanged();

private:
    void readWindow(qint64 offset);
    void rebuildRows();
    /// The window bytes covered by the selection, or empty.
    QByteArray selectedBytes() const;

    PluginServices m_services;
    FileEntry m_entry;
    FileSystemPtr m_fileSystem;

    QByteArray m_window;
    QVariantList m_rows;
    qint64 m_fileSize = -1;
    qint64 m_windowOffset = 0;
    bool m_hasMore = false;

    qint64 m_selectionStart = -1;
    qint64 m_selectionLength = 0;

    QPointer<ReadRangeTask> m_task;
};

class HexPreviewProvider final : public IPreviewProvider
{
public:
    explicit HexPreviewProvider(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview.hex"); }
    QString displayName() const override { return QStringLiteral("Bytes"); }
    /// Below every viewer that understands a format, above the list of facts.
    int priority() const override { return -500; }
    bool canPreview(const FileEntry& entry) const override;
    QUrl viewSource() const override;
    PreviewController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

// --------------------------------------------------------------- image

class ImagePreviewController final : public PreviewController
{
    Q_OBJECT
    Q_PROPERTY(QString source READ source NOTIFY sourceChanged)

public:
    explicit ImagePreviewController(PluginServices services, QObject* parent = nullptr);

    QString source() const { return m_source; }
    void load(const FileEntry& entry) override;

signals:
    void sourceChanged();

private:
    QString m_source;
    LocalCopyProvider* m_copy = nullptr;
};

class ImagePreviewProvider final : public IPreviewProvider
{
public:
    explicit ImagePreviewProvider(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview.image"); }
    QString displayName() const override { return QStringLiteral("Image"); }
    int priority() const override { return 50; }
    bool canPreview(const FileEntry& entry) const override;
    QUrl viewSource() const override;
    PreviewController* createController(QObject* parent) override;

    /// What this Qt build can actually decode, so an unsupported format falls
    /// through to a viewer that can say something useful.
    static QStringList imageSuffixes();

private:
    PluginServices m_services;
};

// --------------------------------------------------------------- table

/// CSV and TSV as a grid. The separator is detected but stays editable,
/// because detection is a guess and the user can see when it guessed wrong.
/// A delimited file as a table.
///
/// The file is imported into a scratch SQLite database rather than parsed into
/// memory, so there is no row limit: paging and filtering are queries, and the
/// answer always covers the whole file rather than whatever prefix happened to
/// be loaded.
class TablePreviewController final : public PreviewController
{
    Q_OBJECT
    Q_PROPERTY(mole::TableModel* table READ table CONSTANT)
    Q_PROPERTY(QString separator READ separator WRITE setSeparator NOTIFY optionsChanged)
    Q_PROPERTY(bool firstRowIsHeader READ firstRowIsHeader WRITE setFirstRowIsHeader NOTIFY optionsChanged)
    Q_PROPERTY(QStringList separatorChoices READ separatorChoices CONSTANT)
    Q_PROPERTY(QString summary READ summary NOTIFY optionsChanged)
    /// True while rows are still arriving. The table is usable throughout --
    /// what is imported so far is what is shown.
    Q_PROPERTY(bool importing READ isImporting NOTIFY importProgress)
    Q_PROPERTY(qint64 importedRows READ importedRows NOTIFY importProgress)

public:
    explicit TablePreviewController(PluginServices services, QObject* parent = nullptr);
    ~TablePreviewController() override;

    TableModel* table() const { return m_table; }
    /// A display form: "," "Tab" ";" "|".
    QString separator() const;
    void setSeparator(const QString& separator);
    bool firstRowIsHeader() const { return m_firstRowIsHeader; }
    void setFirstRowIsHeader(bool isHeader);
    QStringList separatorChoices() const;
    QString summary() const { return m_summary; }
    bool isImporting() const { return m_importing; }
    qint64 importedRows() const { return m_importedRows; }

    /// Puts a block of cells on the clipboard, tab-separated. Called from the
    /// view because only it knows what the user has selected.
    Q_INVOKABLE void copyBlock(int topRow, int leftColumn, int bottomRow, int rightColumn);

    void load(const FileEntry& entry) override;

signals:
    void optionsChanged();
    void importProgress();

private:
    void reimport();
    void updateSummary();

    PluginServices m_services;
    TableModel* m_table = nullptr;
    FileEntry m_entry;
    std::unique_ptr<QTemporaryDir> m_scratch;
    std::unique_ptr<DelimitedStore> m_store;
    QChar m_separator;
    bool m_firstRowIsHeader = true;
    bool m_importing = false;
    qint64 m_importedRows = 0;
    QString m_summary;
    QPointer<ImportDelimitedTask> m_task;
};

// ------------------------------------------------------- database and parquet

/// A SQLite file: its tables, and the selected one as a grid.
///
/// Read in place. A database is already a queryable table, so importing it to
/// page through it would be absurd -- and the file is opened read-only, because
/// previewing something is not a licence to modify it.
class SqlitePreviewController final : public PreviewController
{
    Q_OBJECT
    Q_PROPERTY(mole::TableModel* table READ table CONSTANT)
    /// Table and view names, with a row count each.
    Q_PROPERTY(QVariantList tables READ tables NOTIFY schemaChanged)
    Q_PROPERTY(QString currentTable READ currentTable WRITE setCurrentTable NOTIFY schemaChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY schemaChanged)

public:
    explicit SqlitePreviewController(PluginServices services, QObject* parent = nullptr);
    ~SqlitePreviewController() override;

    TableModel* table() const { return m_table; }
    QVariantList tables() const;
    QString currentTable() const;
    void setCurrentTable(const QString& table);
    QString summary() const { return m_summary; }

    Q_INVOKABLE void copyBlock(int topRow, int leftColumn, int bottomRow, int rightColumn);

    void load(const FileEntry& entry) override;

signals:
    void schemaChanged();

private:
    void refreshSummary();

    PluginServices m_services;
    TableModel* m_table = nullptr;
    std::unique_ptr<SqliteTable> m_database;
    std::unique_ptr<LocalCopyProvider> m_copy;
    QString m_summary;
};

class SqlitePreviewProvider final : public IPreviewProvider
{
public:
    explicit SqlitePreviewProvider(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview.sqlite"); }
    QString displayName() const override { return QStringLiteral("Database"); }
    int priority() const override { return 60; }
    bool canPreview(const FileEntry& entry) const override;
    QUrl viewSource() const override;
    PreviewController* createController(QObject* parent) override;

    static QStringList databaseSuffixes();

private:
    PluginServices m_services;
};

/// A Parquet file as a grid. One dataset, so there is no table list to make.
class ParquetPreviewController final : public PreviewController
{
    Q_OBJECT
    Q_PROPERTY(mole::TableModel* table READ table CONSTANT)
    Q_PROPERTY(QString summary READ summary NOTIFY schemaChanged)
    Q_PROPERTY(QStringList columnTypes READ columnTypes NOTIFY schemaChanged)

public:
    explicit ParquetPreviewController(PluginServices services, QObject* parent = nullptr);
    ~ParquetPreviewController() override;

    TableModel* table() const { return m_table; }
    QString summary() const { return m_summary; }
    QStringList columnTypes() const;

    Q_INVOKABLE void copyBlock(int topRow, int leftColumn, int bottomRow, int rightColumn);

    void load(const FileEntry& entry) override;

signals:
    void schemaChanged();

private:
    PluginServices m_services;
    TableModel* m_table = nullptr;
    std::unique_ptr<ParquetTable> m_file;
    std::unique_ptr<LocalCopyProvider> m_copy;
    QString m_summary;
};

class ParquetPreviewProvider final : public IPreviewProvider
{
public:
    explicit ParquetPreviewProvider(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview.parquet"); }
    QString displayName() const override { return QStringLiteral("Parquet"); }
    int priority() const override { return 60; }
    /// False in a build without Arrow, so the file falls through to the
    /// information viewer instead of opening an empty grid.
    bool canPreview(const FileEntry& entry) const override;
    QUrl viewSource() const override;
    PreviewController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

class TablePreviewProvider final : public IPreviewProvider
{
public:
    explicit TablePreviewProvider(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview.table"); }
    QString displayName() const override { return QStringLiteral("Table"); }
    /// Above the text viewer, which would otherwise claim .csv first.
    int priority() const override { return 100; }
    bool canPreview(const FileEntry& entry) const override;
    QUrl viewSource() const override;
    PreviewController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

// ------------------------------------------------------------ file info

/// The last resort: a file nothing can show, named.
///
/// It used to build nine facts of its own out of `stat()`. They are the generic
/// metadata reader's now, shown in the details panel that every viewer has and
/// this one opens by default -- so a file with a viewer gets them too, which is
/// the whole reason they moved. See ADR-0034.
class FileInfoPreviewController final : public PreviewController
{
    Q_OBJECT
    Q_PROPERTY(QString headline READ headline NOTIFY factsChanged)

public:
    explicit FileInfoPreviewController(PluginServices services, QObject* parent = nullptr);

    QString headline() const { return m_headline; }
    void load(const FileEntry& entry) override;

signals:
    void factsChanged();

private:
    PluginServices m_services;
    QString m_headline;
};

class FileInfoPreviewProvider final : public IPreviewProvider
{
public:
    explicit FileInfoPreviewProvider(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview.fileinfo"); }
    QString displayName() const override { return QStringLiteral("File information"); }
    /// Lowest of all: it accepts everything, so nothing else must lose to it.
    int priority() const override { return -1000; }
    bool canPreview(const FileEntry& entry) const override { return !entry.isDir; }
    /// The details are all this viewer has, so it opens them.
    bool detailsOpenByDefault() const override { return true; }
    QUrl viewSource() const override;
    PreviewController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

} // namespace mole
