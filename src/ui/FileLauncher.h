#pragma once

#include "sdk/PluginServices.h"

#include "core/vfs/VfsTypes.h"
#include "core/vfs/VfsUri.h"

#include <QObject>
#include <QTemporaryDir>

#include <functional>
#include <memory>

namespace mole {

/// Hands a file to whatever the desktop uses to open it.
///
/// Files on a remote or archive drive have no path the desktop can reach, so
/// they are streamed into a scratch directory first and that copy is opened.
class FileLauncher : public QObject
{
    Q_OBJECT

public:
    /// The final "give it to the operating system" step. Replaceable so the
    /// test suite can assert what would have been opened without actually
    /// launching a PDF viewer.
    using OpenHook = std::function<bool(const QString& localPath)>;

    explicit FileLauncher(PluginServices services, QObject* parent = nullptr);
    ~FileLauncher() override;

    void setOpenHook(OpenHook hook);

    /// Opens `uri`, extracting it first when it is not a local file.
    void open(const VfsUri& uri);

signals:
    void opened(const QString& localPath);
    /// **The error is the drive's own only when the drive is what failed**, and
    /// a caller that marks a drive on a failure has to look at the code rather
    /// than at the fact of a failure. This carried a bare reason string, and six
    /// of the eight say nothing about a drive -- no handler, no registered
    /// application, an invalid location, no scratch directory, a full local
    /// disk. The shell wrapped every one as VfsError::IoError and reported it as
    /// a drive failure, so opening a file nobody has a viewer for turned a
    /// perfectly good server's row red and left it saying "Unreachable". See
    /// MOLE-395.
    ///
    /// So: the read's own error when the read failed, `NotFound` when nothing is
    /// mounted for the uri, and a code that cannot mean an unreachable drive for
    /// everything this machine could not do.
    void failed(const QString& uri, const mole::VfsError& error);

public:
    /// Where a file from a drive with no local path is staged before being
    /// handed to the desktop.
    ///
    /// Public so the invariant can be asserted directly: the result is inside
    /// the scratch directory. That is worth having on every platform and not
    /// only where it currently breaks, because the cost of being wrong is
    /// writing over somebody's file.
    QString scratchPathFor(const VfsUri& uri);
    /// The scratch directory itself, once there is one. Empty before the first
    /// staging.
    QString scratchDirectory() const;

private:
    void launch(const QString& localPath, const VfsUri& origin);

    PluginServices m_services;
    OpenHook m_openHook;
    std::unique_ptr<QTemporaryDir> m_scratch;
};

} // namespace mole
