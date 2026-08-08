#include "plugins/archive/ArchiveFileSystem.h"
#include "sdk/PluginApi.h"

namespace mole {

/// Ships archive support as a real, separately built shared library rather
/// than compiling it into the application.
///
/// That is deliberate: it is the proof that the published plugin API is
/// sufficient to add a new kind of drive from outside the core, and it doubles
/// as the template a community plugin can copy.
class ArchivePlugin : public QObject, public IPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MOLE_PLUGIN_IID FILE "archive.json")
    Q_INTERFACES(mole::IPlugin)

public:
    PluginMetadata metadata() const override
    {
        PluginMetadata data;
        data.id = QStringLiteral("mole.archive");
        data.name = QStringLiteral("Archive drives");
        data.version = QStringLiteral("0.1.0");
        data.author = QStringLiteral("Mole");
        data.description
            = QStringLiteral("Mount zip, tar, 7z and other archives as browsable drives (read-only).");
        return data;
    }

    void registerExtensions(PluginRegistry& registry) override
    {
        registry.addFileSystemFactory(std::make_unique<ArchiveFileSystemFactory>());
    }
};

} // namespace mole

#include "ArchivePlugin.moc"
