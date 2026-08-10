#include "plugins/builtin/BuiltinPlugin.h"

#include "plugins/builtin/AlertsFeature.h"
#include "plugins/builtin/AnalysisFeature.h"
#include "plugins/builtin/AnalysisJob.h"
#include "plugins/builtin/AutomationFeature.h"
#include "plugins/builtin/BrowserFeature.h"
#include "plugins/builtin/BulkRenameFeature.h"
#include "plugins/builtin/DuplicatesFeature.h"
#include "plugins/builtin/FileSetsFeature.h"
#include "plugins/builtin/PreviewFeature.h"
#include "plugins/builtin/ReportsFeature.h"
#include "plugins/builtin/SearchFeatures.h"
#include "plugins/builtin/SyncFeature.h"
#include "plugins/builtin/previews/ImageMetadata.h"
#include "plugins/builtin/previews/MetadataReaders.h"
#include "plugins/builtin/previews/PdfPreview.h"
#include "plugins/builtin/previews/PreviewProviders.h"

#include "core/automation/ScheduleStore.h"
#include "core/automation/Scheduler.h"
#include "core/vfs/backends/LocalFileSystem.h"
#include "core/vfs/backends/MemoryFileSystem.h"

namespace mole {

BuiltinPlugin::BuiltinPlugin(QString defaultUri)
    : m_defaultUri(std::move(defaultUri))
{
}

BuiltinPlugin::~BuiltinPlugin() = default;

PluginMetadata BuiltinPlugin::metadata() const
{
    PluginMetadata data;
    data.id = QStringLiteral("mole.builtin");
    data.name = QStringLiteral("Mole built-ins");
    data.version = QStringLiteral("0.1.0");
    data.author = QStringLiteral("Mole");
    data.description = QStringLiteral("Local and in-memory drives, browsing, search and text preview.");
    return data;
}

void BuiltinPlugin::registerExtensions(PluginRegistry& registry)
{
    const PluginServices& services = registry.services();

    registry.addFileSystemFactory(std::make_unique<LocalFileSystemFactory>());
    registry.addFileSystemFactory(std::make_unique<MemoryFileSystemFactory>());
    // Network drives are not here. SFTP, FTP, S3 and WebDAV arrive through the
    // network plugin, which is loaded like any other shared library rather than
    // compiled in -- see docs/adr/0011-network-drives-without-rclone.md.

    // The same workflow registered twice: single pane and dual pane are
    // separate contexts in the new-tab menu but share every line of behaviour.
    registry.addFeature(
        std::make_unique<BrowserFeature>(services, m_defaultUri, BrowserFeature::singlePaneConfig()));
    registry.addFeature(
        std::make_unique<BrowserFeature>(services, m_defaultUri, BrowserFeature::dualPaneConfig()));
    registry.addFeature(std::make_unique<LiveSearchFeature>(services, m_defaultUri));
    registry.addFeature(std::make_unique<IndexSearchFeature>(services));
    registry.addFeature(std::make_unique<PreviewFeature>(services));

    registry.addFeature(std::make_unique<AnalysisFeature>(services));
    AnalysisStore* analysisStore = services.reports;

    // Reports can be put on a clock. The runner is owned here rather than by
    // the feature so it keeps working while no analysis tab is open -- which
    // is the whole point of scheduling one.
    if (analysisStore)
        registry.addFeature(std::make_unique<ReportsFeature>(services, analysisStore));

    registry.addFeature(std::make_unique<BulkRenameFeature>(services));
    registry.addFeature(std::make_unique<DuplicatesFeature>(services));
    registry.addFeature(std::make_unique<SyncFeature>(services));

    if (services.sets)
        registry.addFeature(std::make_unique<FileSetsFeature>(services, services.sets));

    if (services.alerts) {
        registry.addFeature(std::make_unique<AlertsFeature>(services, services.alerts, analysisStore));
    }

    if (services.scheduler) {
        m_analysisJob = std::make_unique<AnalysisJob>(services, analysisStore);
        services.scheduler->registerJob(AnalysisJob::kind(), m_analysisJob.get());
        registry.addFeature(
            std::make_unique<AutomationFeature>(services.scheduler->store(), services.scheduler));
    }

    // Priority decides who wins for a file several of them accept: the table
    // beats the text viewer for .csv, and the file-info fallback loses to
    // everything.
    // Databases and Parquet outrank the table viewer: a .db is not delimited
    // text, and reading it as such would produce nonsense rather than nothing.
    registry.addPreviewProvider(std::make_unique<PdfPreviewProvider>(services));
    registry.addPreviewProvider(std::make_unique<SqlitePreviewProvider>(services));
    registry.addPreviewProvider(std::make_unique<ParquetPreviewProvider>(services));
    registry.addPreviewProvider(std::make_unique<TablePreviewProvider>(services));
    registry.addPreviewProvider(std::make_unique<ImagePreviewProvider>(services));
    registry.addPreviewProvider(std::make_unique<TextPreviewProvider>(services));
    registry.addPreviewProvider(std::make_unique<HexPreviewProvider>(services));
    registry.addPreviewProvider(std::make_unique<FileInfoPreviewProvider>(services));

    // What every file says about itself, whichever viewer shows it.
    registry.addMetadataReader(std::make_unique<ImageMetadataReader>());
    registry.addMetadataReader(std::make_unique<GenericMetadataReader>());
}

} // namespace mole
