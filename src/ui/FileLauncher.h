#pragma once

#include "sdk/PluginServices.h"

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
    void failed(const QString& uri, const QString& reason);

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
