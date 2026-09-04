#include "plugins/network/FtpFileSystem.h"
#include "plugins/network/S3FileSystem.h"
#include "plugins/network/SftpFileSystem.h"
#ifdef MOLE_HAVE_NFS
#include "plugins/network/NfsFileSystem.h"
#endif
#ifdef MOLE_HAVE_SMB
#include "plugins/network/SmbFileSystem.h"
#endif
#include "plugins/network/WebdavFileSystem.h"
#include "sdk/PluginApi.h"

namespace mole {

/// The standard network drives, as one loadable plugin.
///
/// Deliberately a real shared library rather than something compiled into the
/// application. rclone, which this replaces, lived under src/plugins/ and was
/// linked straight into mole_builtin -- so the published plugin API was carrying
/// exactly one shipped user, the archive plugin. Now it carries two, and the
/// second one is the piece most likely to be copied by somebody adding a backend
/// of their own. See docs/adr/0011-network-drives-without-rclone.md.
class NetworkPlugin : public QObject, public IPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MOLE_PLUGIN_IID FILE "network.json")
    Q_INTERFACES(mole::IPlugin)

public:
    PluginMetadata metadata() const override
    {
        PluginMetadata data;
        data.id = QStringLiteral("mole.network");
        data.name = QStringLiteral("Standard network drives");
        data.version = QStringLiteral(MOLE_VERSION);
        data.author = QStringLiteral("Mole");
        // The two optional ones are named only where they were built, because a
        // description listing a drive kind this build does not offer is a
        // description that misleads. It said four when six ship. See MOLE-369.
        QStringList kinds { QStringLiteral("SFTP"), QStringLiteral("FTP"),
            QStringLiteral("S3-compatible object stores"), QStringLiteral("WebDAV") };
#ifdef MOLE_HAVE_SMB
        kinds.append(QStringLiteral("Windows shares"));
#endif
#ifdef MOLE_HAVE_NFS
        kinds.append(QStringLiteral("NFS"));
#endif
        const QString last = kinds.takeLast();
        data.description
            = QStringLiteral("Connect to %1 and %2.").arg(kinds.join(QStringLiteral(", ")), last);
        return data;
    }

    void registerExtensions(PluginRegistry& registry) override
    {
        registry.addFileSystemFactory(std::make_unique<SftpFileSystemFactory>());
        registry.addFileSystemFactory(std::make_unique<FtpFileSystemFactory>());
        registry.addFileSystemFactory(std::make_unique<S3FileSystemFactory>());
        registry.addFileSystemFactory(std::make_unique<WebdavFileSystemFactory>());
#ifdef MOLE_HAVE_SMB
        // Only where Samba's client library was found. A drive kind that is
        // offered and then cannot connect is worse than one that is not offered.
        registry.addFileSystemFactory(std::make_unique<SmbFileSystemFactory>());
#endif
#ifdef MOLE_HAVE_NFS
        // Likewise: only where libnfs was found.
        registry.addFileSystemFactory(std::make_unique<NfsFileSystemFactory>());
#endif
    }
};

} // namespace mole

#include "NetworkPlugin.moc"
