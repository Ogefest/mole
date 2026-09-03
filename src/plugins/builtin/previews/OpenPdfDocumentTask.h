#pragma once

#include "core/tasks/Task.h"

#ifdef MOLE_HAVE_QTPDF

#include <QList>
#include <QPdfDocument>
#include <QSizeF>
#include <QString>

namespace mole {

/// Opens a document to find out what is in it, off the thread that draws.
///
/// `QPdfDocument::load()` parses the cross-reference table, which for a large
/// document is real work -- and the controller did it on the drawing thread, on a
/// path that may be a kernel-mounted share. MOLE-286 moved the *render* off that
/// thread and left the open on it. See MOLE-360.
///
/// What comes back is numbers rather than a document, and that is deliberate.
/// QPdfDocument is not documented as thread-safe, and the controller used to
/// read page sizes off the one it held while a render was reading its own -- so
/// there is no document here to hand over. The page sizes are taken once, here,
/// which is the only thing the controller wanted it for.
class OpenPdfDocumentTask final : public Task
{
    Q_OBJECT

public:
    /// Everything the interface asks a document about.
    struct Contents
    {
        QPdfDocument::Error error = QPdfDocument::Error::None;
        QString title;
        /// One entry per page, in points. The controller reads these while the
        /// view is being drawn, which is why they are values and not a handle.
        QList<QSizeF> pageSizes;
    };

    explicit OpenPdfDocumentTask(QString path, QObject* parent = nullptr);

signals:
    /// Emitted on the UI thread, once.
    void opened(mole::OpenPdfDocumentTask::Contents contents);

protected:
    void run() override;

private:
    QString m_path;
};

} // namespace mole

Q_DECLARE_METATYPE(mole::OpenPdfDocumentTask::Contents)

#endif // MOLE_HAVE_QTPDF
