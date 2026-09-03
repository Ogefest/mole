#pragma once

#include "core/vfs/VfsTypes.h"

#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>

class QIODevice;

#include <memory>

namespace mole::staging {

/// Where a payload too large to hold in memory is put while it is being written,
/// and the one place that decides it.
///
/// **Qt will not refuse a staging directory that is not there, and on some builds
/// it will not even tell you where it looked.** `QDir::tempPath()` hands back
/// whatever `TMPDIR` says without asking whether it exists, and on Fedora's Qt
/// 6.8 it hands back an empty string for a directory that has been removed --
/// whereupon `QTemporaryFile` creates its file in the *filesystem root*. For an
/// ordinary account that fails with `EACCES`, which looks like the right answer
/// for the wrong reason; for root it succeeds, and a download or an upload is
/// then staged where nobody would ever look for it. See MOLE-297 and MOLE-304.
///
/// So every staged payload comes through here, and two things follow. A file is
/// created **in the directory that was asked for** -- the template is set rather
/// than left to Qt to guess from `argv[0]` -- and a directory that is not there
/// is a refusal with a reason rather than a file somewhere else.
///
/// `MOLE_STAGING_DIR` overrides where that is. It is what makes the refusal
/// assertable on any account and any platform, without touching `TMPDIR` and
/// hoping the platform disagrees -- and it is useful in its own right on a
/// machine whose temporary directory is small or on the wrong disk.

/// The directory staging happens in. Not checked here: what is *asked for* is a
/// separate question from whether it can be used, and the callers below say why
/// when it cannot.
QString directory();

/// Opens `file` inside directory(). False means nothing was created, and `why`
/// says what was wrong with the directory.
bool openFile(QTemporaryFile& file, QString* why = nullptr);

/// A scratch directory inside directory(), or nothing with a reason.
std::unique_ptr<QTemporaryDir> makeDirectory(QString* why = nullptr);

/// Writes `bytes` to `path`, whole or not at all.
///
/// Two callers did this in eight lines apiece and neither looked at the result:
/// `FileLauncher`, extracting a file so an external program can open it, and
/// `LocalCopyProvider`, extracting one so a viewer can. A short write -- a
/// scratch directory on a full /tmp -- was invisible, and what was handed on was
/// a truncated file under the name somebody asked for. The image viewer then
/// declined it with "this build's image plugins could not decode it" and the
/// file walked down the ladder for a disk-space problem. Every other writer in
/// the tree checks: the thumbnail cache, the PDF render, both copy engines, the
/// S3 uploader, the curl sink. See MOLE-406.
///
/// Through QSaveFile, so a failure leaves nothing at `path` rather than half a
/// file: the same reason a transfer writes under a working name (ADR-0020).
Result<void> writeWhole(const QString& path, const QByteArray& bytes);

/// The half of writeWhole() that is about the bytes rather than about the file.
///
/// Separate because a short write is the failure worth testing and a real
/// filesystem will not produce one on demand -- a full disk is not something a
/// test can arrange. `name` appears in the message; the device is already open.
Result<void> writeWholeTo(QIODevice& file, const QByteArray& bytes, const QString& name);

} // namespace mole::staging
