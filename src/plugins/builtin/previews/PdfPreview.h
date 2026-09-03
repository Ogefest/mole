#pragma once

#include "sdk/IMetadataReader.h"
#include "sdk/IPreviewProvider.h"

#include <QPointer>
#include <QSet>
#include <QTemporaryDir>

#include <memory>

#ifdef MOLE_HAVE_QTPDF
#include "plugins/builtin/previews/OpenPdfDocumentTask.h"
#include "plugins/builtin/previews/RenderPdfPageTask.h"

#include <QPdfDocument>
#endif

namespace mole {

class LocalCopyProvider;

#ifdef MOLE_HAVE_QTPDF

/// A PDF as pages, rendered one at a time.
///
/// The document is opened read-only -- previewing something is not a licence to
/// modify it -- and a page is rendered only when the view asks for it, because a
/// six-hundred-page scan held as images would be the worst possible way to look at
/// the first page of it. See docs/adr/0004-pdf-previews.md.
class PdfPreviewController final : public PreviewController
{
    Q_OBJECT
    Q_PROPERTY(int pageCount READ pageCount NOTIFY documentChanged)
    /// The page the view is looking at, 0-based, for the position strip.
    Q_PROPERTY(int currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY currentPageChanged)
    Q_PROPERTY(QString title READ title NOTIFY documentChanged)
    /// How many pages of this document have been rendered so far.
    ///
    /// A count rather than a state, and it exists to be read from a binding: a
    /// page is asked for, answered with nothing, and arrives later, so a view that
    /// did not watch something would never ask again. See pageImage().
    Q_PROPERTY(int pagesRendered READ pagesRendered NOTIFY renderChanged)
    /// What to say while a page is being rasterised, empty when none is. The pane
    /// says everything else about the document in its strip; a page that takes a
    /// second has to be visible there rather than looking like a page that is
    /// blank.
    Q_PROPERTY(QString renderNote READ renderNote NOTIFY renderChanged)

public:
    explicit PdfPreviewController(PluginServices services, QObject* parent = nullptr);
    ~PdfPreviewController() override;

    int pageCount() const;
    int currentPage() const { return m_currentPage; }
    void setCurrentPage(int page);
    QString positionText() const;
    QString title() const;
    int pagesRendered() const { return m_pagesRendered; }
    QString renderNote() const;

    /// A `file:` url for `page` at `width` pixels wide, or an empty string while
    /// there is not one yet.
    ///
    /// **Nothing here rasterises.** A page that has not been rendered starts a
    /// task and this answers with nothing; the file arrives later and
    /// `pagesRendered` is what tells a binding to ask again. Asking for the same
    /// page at the same width twice is one render, and asking again once it has
    /// landed is a stat() -- so scrolling back is free and a binding may re-run as
    /// often as it likes.
    Q_INVOKABLE QString pageImage(int page, int width);

    /// Renders asked for and not yet answered, in flight and queued together.
    /// Exposed so a test can hold that stepping off a document abandons what it
    /// had outstanding, which is a claim about the controller and the queue
    /// together and cannot be made about either alone.
    int outstandingRenders() const;
    /// The page's aspect, so a delegate can reserve the right height before the
    /// image exists and the list does not jump as pages arrive.
    Q_INVOKABLE double pageAspect(int page) const;

    void load(const FileEntry& entry) override;

signals:
    void documentChanged();
    void currentPageChanged();
    /// A render landed, or the last one finished. Coarse on purpose: a view asks
    /// again for whatever it is showing rather than being told about one page.
    void renderChanged();

private:
    void openLocalFile(const QString& path);
    /// What the open found, once the task has read it.
    void documentOpened(const OpenPdfDocumentTask::Contents& contents);
    /// Queues `page` at `width` unless it is already asked for, and starts it if
    /// nothing else is rendering.
    void requestRender(int page, int width);
    /// Starts the next queued render, if there is room. One at a time: see
    /// RenderPdfPageTask.
    void startNextRender();
    /// Cancels what is in flight and forgets what is queued. Called when the
    /// reader steps off the document and when this is destroyed.
    void abandonRenders();
    /// The file a page at a width renders into, empty when there is nowhere to put
    /// it. Creates the scratch directory on first use.
    QString targetFor(int page, int width);

    /// One page at one width, which is what a render is keyed on.
    struct PageKey
    {
        int page = 0;
        int width = 0;

        bool operator==(const PageKey& other) const { return page == other.page && width == other.width; }
    };

    PluginServices m_services;
    std::unique_ptr<LocalCopyProvider> m_copy;
    /// One entry per page, in points, as the open recorded them -- and no
    /// document at all. QPdfDocument is not documented as thread-safe and the
    /// renders open their own; holding one here to read page sizes off while a
    /// render read another was a race waiting to be noticed. See MOLE-360.
    QList<QSizeF> m_pageSizes;
    /// The document's own title, empty when it has none.
    QString m_title;
    /// Where rendered pages go. One directory per preview, gone when it closes --
    /// or when the last render writing into it finishes, whichever is later, which
    /// is why this is shared rather than owned outright. See MOLE-290 for the fault
    /// that taught us the difference.
    std::shared_ptr<QTemporaryDir> m_scratch;
    /// The local copy every render opens for itself. Empty until the file lands.
    QString m_documentPath;
    /// The one render allowed to be running, and the ones waiting behind it.
    QPointer<RenderPdfPageTask> m_render;
    QList<PageKey> m_queued;
    /// Pages that could not be rendered, so a binding asking again does not start
    /// the same doomed task for ever.
    QSet<QString> m_unrenderable;
    int m_pagesRendered = 0;
    int m_currentPage = 0;
    QString m_fileName;
};

#endif // MOLE_HAVE_QTPDF

/// Claims `.pdf` -- but only in a build that can actually render one.
///
/// Without `Qt6::Pdf` this provider still exists and still refuses every file, so
/// a PDF falls through to the information viewer rather than opening an empty
/// frame. The image provider sets the same precedent by claiming only what its
/// build can decode.
class PdfPreviewProvider final : public IPreviewProvider
{
public:
    explicit PdfPreviewProvider(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview.pdf"); }
    QString displayName() const override { return QStringLiteral("Document"); }
    /// Above the text viewer, which would otherwise treat a PDF as bytes.
    int priority() const override { return 70; }
    bool canPreview(const FileEntry& entry) const override;
    QUrl viewSource() const override;
    PreviewController* createController(QObject* parent) override;

    /// True when this build can render a PDF at all.
    static bool isAvailable();

private:
    PluginServices m_services;
};

/// What a document says about itself: who wrote it, what wrote it, how many
/// pages.
///
/// Beside the viewer rather than with the other readers, and for the same reason
/// the viewer is here: without `Qt6::Pdf` there is no way to open a PDF at all,
/// so in that build this claims nothing and a document keeps the generic facts.
///
/// **A PDF is opened, not sampled.** A document's trailer is at the end of the
/// file and its metadata is reached through it, so there is no prefix that
/// answers -- which is why this reader takes a local path where there is one and
/// fetches the file where there is not. A PDF on a remote drive is therefore
/// fetched a second time when somebody opens the panel, the viewer having fetched
/// it once. Accepted rather than papered over: a local copy shared between a
/// viewer and a reader is a caching decision of its own.
class PdfMetadataReader final : public IMetadataReader
{
public:
    QString id() const override { return QStringLiteral("mole.metadata.pdf"); }
    int priority() const override { return 100; }
    bool canRead(const FileEntry& entry) const override;
    QList<FileFact> read(const FileEntry& entry, QByteArrayView head, PluginServices services,
        const CancelToken& cancel) const override;

    /// The facts of a document already in memory, for a test that would rather
    /// not go through a drive.
    static QList<FileFact> factsForBytes(const QByteArray& document);
};

} // namespace mole
