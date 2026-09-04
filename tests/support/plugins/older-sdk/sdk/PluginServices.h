#pragma once

namespace mole {

class VfsManager;
class TaskManager;
class IndexDatabase;
class IndexSummary;
class EventBus;
class IPreviewLookup;
class IMetadataLookup;
class IThumbnailLookup;
class Scheduler;
class ChainRegistry;
class AlertStore;
class AnalysisStore;
class FileSetStore;
class Preferences;

/// **A copy of PluginServices as it was two appends ago, for one fixture.**
///
/// tests/support/plugins/ShortServicesPlugin.cpp is compiled with this
/// directory ahead of src/, so that translation unit sees a struct two pointers
/// shorter than the one the host has -- which is exactly the position a plugin
/// built against an older SDK is in. The promise at the top of the real header
/// is that appending is safe; this is what makes that promise assertable rather
/// than assumed. See MOLE-366 and ADR-0098.
///
/// Not generated from the real header: a fixture that followed it would stop
/// being shorter the moment somebody appended again.
///
/// Passing services explicitly rather than exposing globals is what makes both
/// plugins and features testable -- a test hands over an in-memory backend and
/// a throwaway index, and the code under test cannot tell the difference.
struct PluginServices
{
    /// Mount table and backend registry. Use it to resolve a uri to a backend.
    VfsManager* vfs = nullptr;
    /// Submit background work here. Never block the calling thread yourself.
    TaskManager* tasks = nullptr;
    /// The catalogue of previously scanned trees. **Not for the thread that
    /// draws the window** -- it is a database on a disk, and a query from a
    /// property getter or a signal handler is a stall of unmeasured length. Use
    /// `indexSummary` there; this is for tasks.
    IndexDatabase* index = nullptr;
    /// Publish and subscribe to application-wide events.
    EventBus* events = nullptr;
    /// Finds the viewer for a file. Null in a headless context.
    IPreviewLookup* previews = nullptr;
    /// Finds everything that can say what a file is: EXIF, document authors,
    /// media tags. Every reader that claims a file contributes, unlike the
    /// viewer lookup above. Null in a headless context.
    /// See docs/adr/0034-what-a-file-says-about-itself.md.
    IMetadataLookup* metadata = nullptr;
    /// Makes a small picture of a file, for the gallery. One winner per file,
    /// unlike the readers above. Null in a headless context.
    /// See docs/adr/0058-a-file-can-say-what-it-looks-like.md.
    IThumbnailLookup* thumbnails = nullptr;
    /// Repeating work. Register a job kind here to have the host run it on a
    /// schedule; the rules themselves outlive the plugin that made them.
    Scheduler* scheduler = nullptr;
    /// Watched metrics. Register nothing here -- plugins read and write rules
    /// through it, and the host does the checking.
    AlertStore* alerts = nullptr;
    /// Saved directory reports. Shared so a report produced by one tab is the
    /// same one another tab, an alert or the browser sees.
    AnalysisStore* reports = nullptr;
    /// Named collections of files. A plugin can offer to add to one without
    /// knowing what any other plugin does with them.
    FileSetStore* sets = nullptr;
    /// The small things someone has chosen about how they like to work -- how a
    /// viewer behaves for a file type, and whatever else earns a key later. A
    /// plugin reads and writes it without the shell knowing what the keys mean.
    /// See docs/adr/0006-preview-options-and-preferences.md.
    Preferences* preferences = nullptr;

    bool isValid() const { return vfs && tasks && index && events; }
};

} // namespace mole
