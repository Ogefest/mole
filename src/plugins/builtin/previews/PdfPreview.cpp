#include "plugins/builtin/previews/PdfPreview.h"

#include "plugins/builtin/previews/PreviewProviders.h"

#include "core/platform/Staging.h"
#include "core/tasks/TaskManager.h"
#include "core/vfs/VfsManager.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QTemporaryFile>
#include <QUrl>

namespace mole {
namespace {

    QUrl qmlView(const char* name)
    {
        return QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/%1").arg(QLatin1String(name)));
    }

} // namespace

#ifdef MOLE_HAVE_QTPDF

PdfPreviewController::PdfPreviewController(PluginServices services, QObject* parent)
    : PreviewController(parent)
    , m_services(services)
{
}

PdfPreviewController::~PdfPreviewController()
{
    // A render in flight outlives this object and must not be left connected to
    // it. It holds its own document and a share of the scratch directory, so what
    // it goes on to do is write a file nobody will read.
    abandonRenders();
}

int PdfPreviewController::pageCount() const
{
    return m_document ? m_document->pageCount() : 0;
}

void PdfPreviewController::setCurrentPage(int page)
{
    const int clamped = qBound(0, page, qMax(0, pageCount() - 1));
    if (clamped == m_currentPage)
        return;
    m_currentPage = clamped;
    emit currentPageChanged();
}

QString PdfPreviewController::positionText() const
{
    if (pageCount() <= 0)
        return {};
    const QLocale locale;
    return QStringLiteral("Page %1 of %2")
        .arg(locale.toString(m_currentPage + 1), locale.toString(pageCount()));
}

QString PdfPreviewController::title() const
{
    if (!m_document)
        return {};
    // A PDF's own title where it has one, since it is usually more use than the
    // file name; the file name when it does not.
    const QString embedded = m_document->metaData(QPdfDocument::MetaDataField::Title).toString().trimmed();
    return embedded.isEmpty() ? m_fileName : embedded;
}

double PdfPreviewController::pageAspect(int page) const
{
    if (!m_document || page < 0 || page >= pageCount())
        return 1.414; // A4 upright, a reasonable guess for a document
    const QSizeF size = m_document->pagePointSize(page);
    if (size.width() <= 0 || size.height() <= 0)
        return 1.414;
    return size.height() / size.width();
}

QString PdfPreviewController::targetFor(int page, int width)
{
    if (!m_scratch) {
        QString why;
        m_scratch = staging::makeDirectory(&why);
        if (!m_scratch) {
            setErrorText(
                QStringLiteral("Could not make a scratch directory for rendered pages: %1").arg(why));
            return {};
        }
    }
    // Keyed by width as well as page: the same page at the same width is rendered
    // once and read from disk afterwards, and a resized window re-renders rather
    // than scaling something blurry up.
    return QDir(m_scratch->path()).filePath(QStringLiteral("page-%1-%2.png").arg(page).arg(width));
}

QString PdfPreviewController::pageImage(int page, int width)
{
    if (!m_document || page < 0 || page >= pageCount() || width <= 0)
        return {};

    const QString target = targetFor(page, width);
    if (target.isEmpty())
        return {};
    if (QFileInfo::exists(target))
        return QUrl::fromLocalFile(target).toString();

    // A page whose render failed is not asked for again. Without this, clearing
    // the note after a failure would re-run every binding, every binding would
    // ask again, and a damaged page would render for ever.
    if (m_unrenderable.contains(target))
        return {};

    requestRender(page, width);
    return {};
}

void PdfPreviewController::requestRender(int page, int width)
{
    if (m_documentPath.isEmpty())
        return;

    const PageKey key { page, width };
    if (m_render && m_render->page() == page && m_render->width() == width)
        return; // already going
    if (m_queued.contains(key))
        return;

    // A window being dragged asks at a new width, and what was queued at the old
    // one is a render nothing will ever show. The one in flight is left alone: it
    // is already paying for itself, and cancelling it would leave the pane empty
    // for the pages that had arrived.
    m_queued.removeIf([width](const PageKey& queued) { return queued.width != width; });

    m_queued.append(key);
    startNextRender();
    emit renderChanged();
}

void PdfPreviewController::startNextRender()
{
    if (m_render || m_queued.isEmpty() || m_documentPath.isEmpty())
        return;

    const PageKey key = m_queued.takeFirst();
    const QString target = targetFor(key.page, key.width);
    if (target.isEmpty())
        return;

    const QSize size(key.width, qMax(1, qRound(key.width * pageAspect(key.page))));
    auto* task = new RenderPdfPageTask(m_documentPath, m_scratch, target, key.page, size);
    m_render = task;
    // `this` as the context, so a controller that goes away takes the connection
    // with it; the task itself holds nothing of ours but a share of the scratch
    // directory, and finishes into it harmlessly.
    connect(task, &Task::finished, this, [this, task, target] {
        if (m_render != task)
            return;
        m_render.clear();
        if (task->rendered()) {
            ++m_pagesRendered;
        } else if (task->state() == Task::State::Failed) {
            m_unrenderable.insert(target);
            setErrorText(task->error().message);
        }
        startNextRender();
        emit renderChanged();
    });
    m_services.tasks->submit(task);
}

void PdfPreviewController::abandonRenders()
{
    m_queued.clear();
    if (m_render) {
        // The task runs out on its own -- it holds its own document and a share of
        // the scratch directory, so there is nothing to wait for and nothing it can
        // pull down. Detached first: whatever it reports now is about a document
        // nobody is looking at.
        m_render->disconnect(this);
        m_render->requestCancel();
        m_render.clear();
    }
}

int PdfPreviewController::outstandingRenders() const
{
    return (m_render ? 1 : 0) + int(m_queued.size());
}

QString PdfPreviewController::renderNote() const
{
    if (!m_render)
        return {};
    const int waiting = int(m_queued.size());
    if (waiting > 0) {
        return QStringLiteral("Rendering page %1, %2 to follow")
            .arg(QLocale().toString(m_render->page() + 1), QLocale().toString(waiting));
    }
    return QStringLiteral("Rendering page %1…").arg(QLocale().toString(m_render->page() + 1));
}

void PdfPreviewController::openLocalFile(const QString& path)
{
    // Kept, because every render opens this file for itself rather than sharing
    // the document below.
    m_documentPath = path;
    m_document = std::make_unique<QPdfDocument>();
    const QPdfDocument::Error error = m_document->load(path);
    setLoading(false);

    if (error != QPdfDocument::Error::None) {
        m_document.reset();
        m_documentPath.clear();
        // Said plainly rather than as an enum: a reader wants to know whether the
        // file is broken or simply locked.
        switch (error) {
        case QPdfDocument::Error::IncorrectPassword:
            setErrorText(QStringLiteral("This document is password-protected"));
            break;
        case QPdfDocument::Error::UnsupportedSecurityScheme:
            setErrorText(QStringLiteral("This document uses a security scheme Qt cannot open"));
            break;
        case QPdfDocument::Error::FileNotFound:
            setErrorText(QStringLiteral("The file is no longer there"));
            break;
        case QPdfDocument::Error::InvalidFileFormat:
            setErrorText(QStringLiteral("This is not a PDF, or it is damaged beyond reading"));
            break;
        default:
            setErrorText(QStringLiteral("The document could not be opened"));
            break;
        }
        emit documentChanged();
        return;
    }

    m_currentPage = 0;
    emit documentChanged();
    emit currentPageChanged();
}

void PdfPreviewController::load(const FileEntry& entry)
{
    // What is in flight goes first, before the document and the directory it was
    // rendering into are let go of.
    abandonRenders();
    m_document.reset();
    m_scratch.reset();
    m_documentPath.clear();
    m_unrenderable.clear();
    m_pagesRendered = 0;
    m_currentPage = 0;
    m_fileName = entry.name;
    setErrorText({});
    emit documentChanged();
    emit renderChanged();

    // A renderer needs a file. Anything not on local disk is streamed into a
    // scratch copy first, which is why LocalCopyProvider exists.
    setLoading(true);
    m_copy = std::make_unique<LocalCopyProvider>(m_services);
    connect(m_copy.get(), &LocalCopyProvider::ready, this,
        [this](const QString& fileUrl) { openLocalFile(QUrl(fileUrl).toLocalFile()); });
    connect(m_copy.get(), &LocalCopyProvider::failed, this, [this](const QString& reason) {
        setLoading(false);
        setErrorText(reason);
        emit documentChanged();
    });
    m_copy->request(entry.uri);
}

#endif // MOLE_HAVE_QTPDF

// ------------------------------------------------------------------ provider

PdfPreviewProvider::PdfPreviewProvider(PluginServices services)
    : m_services(services)
{
}

bool PdfPreviewProvider::isAvailable()
{
#ifdef MOLE_HAVE_QTPDF
    return true;
#else
    return false;
#endif
}

bool PdfPreviewProvider::canPreview(const FileEntry& entry) const
{
    if (entry.isDir || !isAvailable())
        return false;
    return entry.uri.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
}

QUrl PdfPreviewProvider::viewSource() const
{
    return qmlView("PdfPreview.qml");
}

PreviewController* PdfPreviewProvider::createController(QObject* parent)
{
#ifdef MOLE_HAVE_QTPDF
    return new PdfPreviewController(m_services, parent);
#else
    Q_UNUSED(parent);
    return nullptr;
#endif
}

// ---------------------------------------------------------------- metadata

bool PdfMetadataReader::canRead(const FileEntry& entry) const
{
    if (entry.isDir || !PdfPreviewProvider::isAvailable())
        return false;
    return entry.mimeType == QLatin1String("application/pdf")
        || entry.uri.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0;
}

#ifdef MOLE_HAVE_QTPDF

namespace {

    /// The facts of an open document. Shared by the two ways of opening one.
    QList<FileFact> factsOf(QPdfDocument& document, QPdfDocument::Error status)
    {
        QList<FileFact> facts;

        // One row rather than eight empty ones: a document that cannot be read
        // has one thing to say and it is not its title.
        switch (status) {
        case QPdfDocument::Error::None:
            break;
        case QPdfDocument::Error::IncorrectPassword:
            return { { QStringLiteral("Document"), QStringLiteral("encrypted — needs a password") } };
        default:
            return { { QStringLiteral("Document"), QStringLiteral("cannot be read") } };
        }

        const auto append = [&facts, &document](QPdfDocument::MetaDataField field, const QString& label) {
            const QVariant value = document.metaData(field);
            if (value.typeId() == QMetaType::QDateTime) {
                const QDateTime when = value.toDateTime();
                if (when.isValid())
                    facts.append({ label, QLocale().toString(when, QLocale::ShortFormat) });
                return;
            }
            const QString text = value.toString().trimmed();
            if (!text.isEmpty())
                facts.append({ label, text,
                    label == QLatin1String("Author")      ? QStringLiteral("doc.author")
                        : label == QLatin1String("Title") ? QStringLiteral("doc.title")
                                                          : QString() });
        };

        append(QPdfDocument::MetaDataField::Title, QStringLiteral("Title"));
        append(QPdfDocument::MetaDataField::Author, QStringLiteral("Author"));
        append(QPdfDocument::MetaDataField::Subject, QStringLiteral("Subject"));
        append(QPdfDocument::MetaDataField::Keywords, QStringLiteral("Keywords"));
        append(QPdfDocument::MetaDataField::Creator, QStringLiteral("Created with"));
        append(QPdfDocument::MetaDataField::Producer, QStringLiteral("Produced by"));
        append(QPdfDocument::MetaDataField::CreationDate, QStringLiteral("Created"));
        append(QPdfDocument::MetaDataField::ModificationDate, QStringLiteral("Modified"));

        // "How many pages" is the question people actually ask of a PDF, and how
        // big a page is answers the other one.
        if (document.pageCount() > 0) {
            facts.append({ QStringLiteral("Pages"), QString::number(document.pageCount()),
                QStringLiteral("doc.pages"), double(document.pageCount()) });
            const QSizeF points = document.pagePointSize(0);
            if (!points.isEmpty()) {
                // A point is 1/72 inch; millimetres is what a page size is
                // ordinarily said in.
                facts.append({ QStringLiteral("Page size"),
                    QStringLiteral("%1 × %2 mm")
                        .arg(qRound(points.width() * 25.4 / 72.0))
                        .arg(qRound(points.height() * 25.4 / 72.0)) });
            }
        }
        return facts;
    }

} // namespace

QList<FileFact> PdfMetadataReader::factsForBytes(const QByteArray& document)
{
    // Staged to a file rather than handed a QIODevice: the device overload
    // reports through error() once its asynchronous load has got somewhere,
    // while a path returns the status. It is also what the viewer does with a
    // remote document, so the two agree about what opening one means.
    QTemporaryFile staged;
    if (!staging::openFile(staged))
        return {};
    if (staged.write(document) != document.size())
        return {};
    staged.flush();

    QPdfDocument opened;
    const QPdfDocument::Error status = opened.load(staged.fileName());
    return factsOf(opened, status);
}

QList<FileFact> PdfMetadataReader::read(
    const FileEntry& entry, QByteArrayView head, PluginServices services, const CancelToken& cancel) const
{
    Q_UNUSED(head);
    if (cancel.isCancelled())
        return {};

    // A local file is opened where it lies -- QPdfDocument reads what it needs
    // and no more, which for the metadata is the trailer and one object.
    const QString localPath = entry.uri.toLocalPath();
    if (!localPath.isEmpty()) {
        QPdfDocument document;
        const QPdfDocument::Error status = document.load(localPath);
        return factsOf(document, status);
    }

    // Anywhere else there is no seeking to a trailer, so the document is
    // fetched. See the note on the class: this is the second fetch, and it
    // happens only because somebody opened the panel.
    if (!services.vfs)
        return {};
    FileSystemPtr fs = services.vfs->resolve(entry.uri);
    if (!fs)
        return {};
    Result<std::unique_ptr<QIODevice>> opened = fs->openRead(entry.uri);
    if (!opened.ok() || !opened.value())
        return {};

    const QByteArray bytes = opened.value()->readAll();
    if (cancel.isCancelled())
        return {};
    return factsForBytes(bytes);
}

#else // MOLE_HAVE_QTPDF

QList<FileFact> PdfMetadataReader::factsForBytes(const QByteArray& document)
{
    Q_UNUSED(document);
    return {};
}

QList<FileFact> PdfMetadataReader::read(
    const FileEntry& entry, QByteArrayView head, PluginServices services, const CancelToken& cancel) const
{
    Q_UNUSED(entry);
    Q_UNUSED(head);
    Q_UNUSED(services);
    Q_UNUSED(cancel);
    return {};
}

#endif // MOLE_HAVE_QTPDF

} // namespace mole
