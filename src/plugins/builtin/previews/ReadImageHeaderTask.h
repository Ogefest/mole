#pragma once

#include "core/tasks/Task.h"

#include <QSize>
#include <QString>

namespace mole {

/// Reads an image's header, off the thread that draws.
///
/// What it answers decides whether 1:1 is offered: an image whose decode would
/// ask for more than this build of Qt will allocate cannot be shown at full
/// size, and a greyed button with no explanation reads as the application being
/// broken.
///
/// A task because the read is not as cheap as it looks. QImageReader::size()
/// reads a header for a JPEG or a PNG and **parses the whole document** for an
/// SVG, and the file it opens is a local path -- which since MOLE-286 means
/// either a scratch copy or a kernel-mounted NFS or SMB path, because
/// LocalCopyProvider answers a local uri synchronously. So it ran on the drawing
/// thread, over a network, on a format that parses. See MOLE-360.
class ReadImageHeaderTask final : public Task
{
    Q_OBJECT

public:
    explicit ReadImageHeaderTask(QString path, QObject* parent = nullptr);

signals:
    /// Emitted on the UI thread, once. An invalid `pixels` means the handler
    /// would not say -- which is not a failure: it leaves 1:1 offered, and an
    /// attempt that then fails is caught where it fails.
    void headerRead(QSize pixels, int bitsPerPixel);

protected:
    void run() override;

private:
    QString m_path;
};

} // namespace mole
