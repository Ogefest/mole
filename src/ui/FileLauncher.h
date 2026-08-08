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

private:
    void launch(const QString& localPath, const VfsUri& origin);
    QString scratchPathFor(const VfsUri& uri);

    PluginServices m_services;
    OpenHook m_openHook;
    std::unique_ptr<QTemporaryDir> m_scratch;
};

} // namespace mole
