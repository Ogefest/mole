#include "plugins/builtin/previews/RenderPdfPageTask.h"

#ifdef MOLE_HAVE_QTPDF

#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <QPdfDocument>
#include <QPdfDocumentRenderOptions>
#include <QThread>

namespace mole {

RenderPdfPageTask::RenderPdfPageTask(QString documentPath, std::shared_ptr<QTemporaryDir> scratch,
    QString target, int page, QSize size, QObject* parent)
    : Task(QStringLiteral("Rendering page %1 of %2").arg(page + 1).arg(QFileInfo(documentPath).fileName()),
          parent)
    , m_documentPath(std::move(documentPath))
    , m_scratch(std::move(scratch))
    , m_target(std::move(target))
    , m_page(page)
    , m_size(size)
{
    // One of a crowd: a six-hundred-page scan scrolled through is six hundred of
    // these and none of them is a job anybody remembers starting. Not background
    // though -- looking at the page is exactly asking for it. See ADR-0064.
    setOneOfMany(true);
}

void RenderPdfPageTask::run()
{
    m_ranOn = QThread::currentThread();
    if (isCancelRequested())
        return;

    QPdfDocument document;
    const QPdfDocument::Error status = document.load(m_documentPath);
    if (status != QPdfDocument::Error::None) {
        // Said as a failure rather than as an empty answer: the controller opened
        // this document a moment ago, so a copy that cannot be opened now is the
        // file having gone out from under the preview.
        fail(VfsError::make(VfsError::IoError, QStringLiteral("The document could not be opened")));
        return;
    }
    if (m_page < 0 || m_page >= document.pageCount()) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("There is no page %1").arg(m_page + 1)));
        return;
    }
    if (isCancelRequested())
        return;

    const QImage rendered = document.render(m_page, m_size, QPdfDocumentRenderOptions {});
    // Checked after the render as well as before it, because the render is the
    // part that takes the time -- a reader who has moved on gets no file written
    // and no answer delivered.
    if (isCancelRequested())
        return;
    if (rendered.isNull()) {
        fail(VfsError::make(
            VfsError::IoError, QStringLiteral("Page %1 could not be rendered").arg(m_page + 1)));
        return;
    }

    // Encoded here too. A PNG of a page at reading width is a megabyte or so of
    // deflate, which is the same kind of work as the render and has no more
    // business on the thread that draws.
    //
    // Through a QSaveFile, so a file that exists is a whole file. The controller
    // answers for a page on the strength of the file being there and the view
    // loads it the moment it is, so a PNG written in place is one that can be read
    // half-finished -- which arrives as a page that failed to decode, on a machine
    // fast enough to look. This is not a hypothetical: writing in place is what
    // the first version of MOLE-286 did, and the existing render test caught it.
    QSaveFile file(m_target);
    if (!file.open(QIODevice::WriteOnly)) {
        fail(VfsError::make(VfsError::IoError, QStringLiteral("Could not write a rendered page")));
        return;
    }
    if (!rendered.save(&file, "PNG")) {
        file.cancelWriting();
        fail(VfsError::make(VfsError::IoError, QStringLiteral("Could not write a rendered page")));
        return;
    }
    if (!file.commit()) {
        fail(VfsError::make(VfsError::IoError, QStringLiteral("Could not write a rendered page")));
        return;
    }
    m_rendered = true;
}

} // namespace mole

#endif // MOLE_HAVE_QTPDF
