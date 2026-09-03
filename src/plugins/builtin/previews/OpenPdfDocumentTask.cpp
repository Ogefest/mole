#include "plugins/builtin/previews/OpenPdfDocumentTask.h"

#ifdef MOLE_HAVE_QTPDF

#include <QFileInfo>

namespace mole {

OpenPdfDocumentTask::OpenPdfDocumentTask(QString path, QObject* parent)
    : Task(QStringLiteral("Open %1").arg(QFileInfo(path).fileName()), parent)
    , m_path(std::move(path))
{
    setBackground(true);
}

void OpenPdfDocumentTask::run()
{
    Contents contents;

    // Its own document, taken apart again before this returns. Nothing of it
    // leaves this thread.
    QPdfDocument document;
    contents.error = document.load(m_path);
    if (contents.error != QPdfDocument::Error::None) {
        emit opened(contents);
        return;
    }

    contents.title = document.metaData(QPdfDocument::MetaDataField::Title).toString().trimmed();
    const int pages = document.pageCount();
    contents.pageSizes.reserve(pages);
    for (int page = 0; page < pages; ++page) {
        if (isCancelRequested())
            return;
        contents.pageSizes.append(document.pagePointSize(page));
    }

    emit opened(contents);
    setStatusText(QStringLiteral("%1 page(s)").arg(pages));
}

} // namespace mole

#endif // MOLE_HAVE_QTPDF
