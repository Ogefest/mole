#include "plugins/builtin/previews/PdfPreview.h"

#include "plugins/builtin/previews/PreviewProviders.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
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

} // namespace mole
