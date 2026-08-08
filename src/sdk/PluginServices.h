#pragma once

namespace mole {

class VfsManager;
class TaskManager;
class IndexDatabase;
class EventBus;
class IPreviewLookup;
class Scheduler;
class AlertStore;
class AnalysisStore;
class FileSetStore;
class Preferences;

/// The host services a plugin is allowed to use.
///
/// This struct is part of the plugin ABI surface: fields may be appended, but
/// never removed or reordered without bumping kPluginApiVersion.
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
    /// The catalogue of previously scanned trees.
    IndexDatabase* index = nullptr;
    /// Publish and subscribe to application-wide events.
    EventBus* events = nullptr;
    /// Finds the viewer for a file. Null in a headless context.
    IPreviewLookup* previews = nullptr;
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
