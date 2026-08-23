#pragma once

#include "core/tasks/Task.h"

#ifdef MOLE_HAVE_QTPDF

#include <QSize>
#include <QString>
#include <QTemporaryDir>

#include <memory>

class QThread;

namespace mole {

/// Rasterises one page of a document into a PNG file, on a pool thread.
///
/// The *output* of a render is bounded -- it is the width the view asked for --
/// but the *work* is not: a page is proportional to what is on it, and one large
/// vector drawing, a map or a CAD plot, takes as long as its paths take. Done on
/// the thread that draws, that is the window not answering for as long as the page
/// takes, with nothing able to cancel it. See MOLE-286.
///
/// **Its own document, opened here, and that is the point.** `QPdfDocument` is not
/// documented as thread-safe, and the controller reads page sizes and metadata off
/// the one it holds while the view is being drawn -- so a single document between
/// the two would be a race. It also means this task holds nothing the controller
/// owns except the scratch directory it writes into, which it holds a share of, so
/// stepping off a document cannot pull the ground out from under a render already
/// running. Opening the file again costs a parse of the cross-reference table,
/// which is work in the one place where work is free.
class RenderPdfPageTask final : public Task
{
    Q_OBJECT

public:
    /// `scratch` is shared rather than borrowed: the reader may step off the
    /// document while this is still going, and whichever of the two lets go last
    /// is the one that deletes the directory.
    RenderPdfPageTask(QString documentPath, std::shared_ptr<QTemporaryDir> scratch, QString target, int page,
        QSize size, QObject* parent = nullptr);

    int page() const { return m_page; }
    int width() const { return m_size.width(); }
    /// Where the PNG was asked to go, whether or not it got there.
    QString target() const { return m_target; }
    /// True once the file at target() exists and holds the page.
    bool rendered() const { return m_rendered; }
    /// The thread run() executed on, so a test can hold the house rule that
    /// nothing rasterises on the thread that draws.
    QThread* ranOn() const { return m_ranOn; }

protected:
    void run() override;

private:
    QString m_documentPath;
    std::shared_ptr<QTemporaryDir> m_scratch;
    QString m_target;
    int m_page = 0;
    QSize m_size;
    bool m_rendered = false;
    QThread* m_ranOn = nullptr;
};

} // namespace mole

#endif // MOLE_HAVE_QTPDF
