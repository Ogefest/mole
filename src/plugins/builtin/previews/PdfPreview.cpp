#include "plugins/builtin/previews/PdfPreview.h"

#include "plugins/builtin/previews/PreviewProviders.h"

#include "core/vfs/VfsManager.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QTemporaryFile>
#include <QUrl>

#ifdef MOLE_HAVE_QTPDF
#include <QImage>
#include <QPdfDocumentRenderOptions>
#endif

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

PdfPreviewController::~PdfPreviewController() = default;

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

QString PdfPreviewController::pageImage(int page, int width)
{
    if (!m_document || page < 0 || page >= pageCount() || width <= 0)
        return {};
    if (!m_scratch) {
        m_scratch = std::make_unique<QTemporaryDir>();
        if (!m_scratch->isValid()) {
            setErrorText(QStringLiteral("Could not create a scratch directory for rendered pages"));
            return {};
        }
    }

    // Keyed by width as well as page: the same page at the same width is rendered
    // once and read from disk afterwards, and a resized window re-renders rather
    // than scaling something blurry up.
    const QString target
        = QDir(m_scratch->path()).filePath(QStringLiteral("page-%1-%2.png").arg(page).arg(width));
    if (QFileInfo::exists(target))
        return QUrl::fromLocalFile(target).toString();

    const double aspect = pageAspect(page);
    const QSize size(width, qMax(1, qRound(width * aspect)));
    const QImage rendered = m_document->render(page, size, QPdfDocumentRenderOptions {});
    if (rendered.isNull()) {
        setErrorText(QStringLiteral("Page %1 could not be rendered").arg(page + 1));
        return {};
    }
    if (!rendered.save(target, "PNG")) {
        setErrorText(QStringLiteral("Could not write a rendered page"));
        return {};
    }
    return QUrl::fromLocalFile(target).toString();
}

void PdfPreviewController::openLocalFile(const QString& path)
{
    m_document = std::make_unique<QPdfDocument>();
    const QPdfDocument::Error error = m_document->load(path);
    setLoading(false);

    if (error != QPdfDocument::Error::None) {
        m_document.reset();
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
    m_document.reset();
    m_scratch.reset();
    m_currentPage = 0;
    m_fileName = entry.name;
    setErrorText({});
    emit documentChanged();

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
    if (!staged.open())
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
