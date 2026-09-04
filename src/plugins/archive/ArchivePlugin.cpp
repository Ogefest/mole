#include "plugins/archive/ArchiveFileSystem.h"
#include "plugins/archive/CompressArchiver.h"
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
        data.version = QStringLiteral(MOLE_VERSION);
        data.author = QStringLiteral("Mole");
        data.description = QStringLiteral(
            "Mount zip, tar, 7z and other archives as browsable drives, and pack files into new ones.");
        return data;
    }

    void registerExtensions(PluginRegistry& registry) override
    {
        registry.addFileSystemFactory(std::make_unique<ArchiveFileSystemFactory>());
        // Reading an archive and writing one are two contributions from one
        // plugin: a drive to browse what is inside, and an archiver the compress
        // dialog is filled in from. The shell knows neither by name.
        registry.addArchiver(std::make_unique<CompressArchiver>(registry.services()));
    }
};

} // namespace mole

#include "ArchivePlugin.moc"
