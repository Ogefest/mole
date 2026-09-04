#include "core/tasks/ReadRangeTask.h"

namespace mole {
namespace {

    /// How far back to look for a line break when snapping a window's start. A
    /// file with no newline in 64 kB is not line-structured, and the window is
    /// taken as asked rather than scanning for something that is not there.
    constexpr qint64 kLineScanLimit = 64 * 1024;

} // namespace

ReadRangeTask::ReadRangeTask(
    FileSystemPtr fileSystem, VfsUri target, qint64 offset, qint64 length, QObject* parent)
    : Task(QStringLiteral("Read %1").arg(target.fileName()), parent)
    , m_fileSystem(std::move(fileSystem))
    , m_target(std::move(target))
    , m_offset(std::max<qint64>(0, offset))
    , m_length(std::max<qint64>(0, length))
{
    noteRunsOn(m_fileSystem);
    // One of a crowd: a preview of anything large is a run of ranged reads, and a hex
    // view scrolled is one per screenful. See Task::isOneOfMany() and ADR-0064.
    setOneOfMany(true);
}

void ReadRangeTask::run()
{
    if (!m_fileSystem) {
        fail(VfsError::make(VfsError::NotFound, QStringLiteral("Nothing is mounted for this file")));
        return;
    }

    if (Result<FileEntry> stat = m_fileSystem->stat(m_target); stat.ok())
        m_fileSize = stat.value().size;

    if (m_offset > 0 && !m_fileSystem->capabilities().testFlag(VfsCapability::RandomAccessRead)) {
        fail(VfsError::make(
            VfsError::NotSupported, QStringLiteral("This drive can only be read from the beginning")));
        return;
    }

    Result<std::unique_ptr<QIODevice>> opened = m_fileSystem->openRead(m_target);
    if (!opened.ok()) {
        fail(opened.error());
        return;
    }

    std::unique_ptr<QIODevice> device = std::move(opened.value());
    if (!device) {
        fail(VfsError::make(VfsError::IoError, QStringLiteral("The file could not be opened")));
        return;
    }

    m_actualOffset = m_offset;

    // Back up to just after the previous newline so the window starts on a
    // whole line. Without this, paging through a log shows a severed first
    // line at every step.
    if (m_alignToLines && m_offset > 0) {
        const qint64 scanFrom = std::max<qint64>(0, m_offset - kLineScanLimit);
        if (device->seek(scanFrom)) {
            const QByteArray lead = device->read(m_offset - scanFrom);
            const int breakAt = lead.lastIndexOf('\n');
            if (breakAt >= 0)
                m_actualOffset = scanFrom + breakAt + 1;
        }
    }

    if (!device->seek(m_actualOffset)) {
        fail(VfsError::make(
            VfsError::IoError, QStringLiteral("Could not seek to %1 in this file").arg(m_actualOffset)));
        return;
    }

    // Snapping the start backwards must not shorten the window: the caller
    // asked for the bytes up to offset+length, and a window that stops short of
    // that could never reach the end of the file -- which is what made the
    // final window unreachable.
    const qint64 wanted = m_length + (m_offset - m_actualOffset);

    // One byte more than asked for, purely to learn whether anything follows
    // when the backend could not tell us the size.
    QByteArray window = device->read(wanted + 1);
    if (isCancelRequested())
        return;

    const bool moreFromProbe = window.size() > wanted;
    if (moreFromProbe)
        window.chop(window.size() - wanted);

    // "Is there anything after this window" is a question about the file, not
    // about the read. Answering it from the probe alone made the final window
    // unreachable: snapping the start back to a line boundary left more than
    // one window's worth ahead, so the probe always said yes and the trailing
    // partial line was always trimmed -- so the end never arrived.
    const bool reachedEnd = m_fileSize >= 0 ? m_actualOffset + window.size() >= m_fileSize : !moreFromProbe;

    if (m_alignToLines && !reachedEnd) {
        // Drop a trailing partial line for the same reason as the leading one,
        // unless that would leave nothing at all to show.
        const int lastBreak = window.lastIndexOf('\n');
        if (lastBreak > 0)
            window.truncate(lastBreak + 1);
    }

    m_hasMore = !reachedEnd;

    m_contents = std::move(window);
    setProgress(100);
    setStatusText(QStringLiteral("%1 bytes from offset %2").arg(m_contents.size()).arg(m_actualOffset));
}

} // namespace mole
