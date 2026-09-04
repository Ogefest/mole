# Writing a plugin

**A plugin is built inside a Mole checkout today, and that is the first thing to
know.** `mole_sdk` and `mole_core` are static libraries that exist only in a
configured build tree: no header under `src/sdk` or `src/core` is installed,
there is no export set and no `mole_sdkConfig.cmake`, and every SDK header needs
core headers behind it. So the way to write a plugin now is to clone Mole, put
your plugin under `src/plugins/`, and add it to `src/plugins/CMakeLists.txt` the
way the two shipped ones are added. The ABI is per build: a plugin belongs to the
tree it was built in.
[ADR-0099](adr/0099-a-plugin-is-built-in-tree-and-the-abi-is-per-build.md) records
why it is that way, what it costs, and what making the SDK installable would
take.

A plugin is a shared library that implements `mole::IPlugin`. Through
`PluginRegistry` it can contribute six things — drives, tabs, previews, metadata
readers, thumbnailers and menu entries — and two more through
`registry.services()`: chain step kinds and scheduled job kinds. Nothing else in
the host is reachable, which is what makes the API a contract rather than an
invitation to poke around.

The archive plugin in `src/plugins/archive/` is a working example built exactly
this way — copy it.

## The skeleton

```cpp
#include "sdk/PluginApi.h"

class GitLabPlugin : public QObject, public mole::IPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MOLE_PLUGIN_IID FILE "gitlab.json")
    Q_INTERFACES(mole::IPlugin)

public:
    mole::PluginMetadata metadata() const override
    {
        mole::PluginMetadata data;
        data.id = QStringLiteral("org.example.gitlab");  // reverse-DNS, never changes
        data.name = QStringLiteral("GitLab drives");
        data.version = QStringLiteral("1.0.0");
        return data;                                     // apiVersion defaults correctly
    }

    void registerExtensions(mole::PluginRegistry& registry) override
    {
        registry.addFileSystemFactory(std::make_unique<GitLabFsFactory>(registry.services()));
        registry.addFeature(std::make_unique<BranchDiffFeature>(registry.services()));
    }
};

#include "GitLabPlugin.moc"
```

`registerExtensions()` runs once at startup. Do registration only — no I/O, no
threads, no dialogs. Anything expensive belongs in a `Task` started later.

### CMake

Added to `src/plugins/CMakeLists.txt`, inside the Mole tree — see the first
paragraph:

```cmake
qt_add_plugin(gitlab_plugin SHARED CLASS_NAME GitLabPlugin)
# Qt 6.4's qt_add_plugin() ignores trailing source arguments.
target_sources(gitlab_plugin PRIVATE GitLabPlugin.cpp GitLabFileSystem.cpp)
target_link_libraries(gitlab_plugin PRIVATE mole_sdk)
set_target_properties(gitlab_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
```

`CLASS_NAME` is the class's *unqualified* name even when the class is in a
namespace: it is only ever the argument of a `Q_IMPORT_PLUGIN(...)`, and the
symbol moc exports for a plugin class in a namespace is
`qt_static_plugin_GitLabPlugin` — no namespace in it. Qt 6.10 refuses to
configure when it is given a qualified one.

**Where the host looks**, in this order:

- `<executable dir>/plugins`, so a build tree works without installing
- `<prefix>/lib/mole/plugins` **and** `<prefix>/lib64/mole/plugins`, resolved
  relative to the binary rather than baked in — both, because GNUInstallDirs
  answers `lib64` on an RPM distribution and searching only `lib` once shipped an
  `.rpm` whose plugins nothing looked for
- the user's data directory (`AppDataLocation/plugins`)
- anything in `MOLE_PLUGIN_PATH`, which is the escape hatch for development

Symbolic links are followed, so linking a build-tree `.so` into
`MOLE_PLUGIN_PATH` works. A file that is not a library is skipped; a library that
cannot be loaded is reported rather than skipped in silence, and `mole --plugins`
prints what was found and what failed.

## Adding a drive

Implement `IFileSystem` (the operations) and `IFileSystemFactory` (how to build
one from a config).

```cpp
class GitLabFileSystem final : public mole::IFileSystem {
public:
    QString scheme() const override { return QStringLiteral("gitlab"); }

    mole::VfsCapabilities capabilities() const override
    {
        return mole::VfsCapability::Read;   // advertise only what you implement
    }

    mole::Result<mole::FileEntryList> list(const mole::VfsUri& dir,
                                         const mole::CancelToken& cancel) override
    {
        // Blocking is fine and expected -- see "threading" below.
        if (cancel.isCancelled())
            return mole::VfsError::make(mole::VfsError::Cancelled, QStringLiteral("cancelled"));
        ...
    }
};
```

**Threading.** Every method is called from a worker thread, never the UI
thread, so write plain blocking code. In exchange, two obligations: be safe to
call concurrently from several workers (or serialise internally with a mutex),
and poll the `CancelToken` in anything slow.

**Errors.** Map your backend's failures onto `VfsError::Code`. The UI shows the
message and branches on the code, and it must not have to know which backend it
is talking to. `NotFound` has to mean the same thing everywhere.

**Optional operations.** The defaults return `NotSupported`, so a backend can
start read-only and grow. Advertise the matching capability when you override
one.

**Prove it.** Point the shared conformance suite at your backend:

```cpp
void TestGitLabFs::conformance()
{
    mole::test::ConformanceContext context;
    context.fileSystem = std::make_shared<GitLabFileSystem>(...);
    context.root = ...;
    context.seedFile = [&](const QString& path, const QByteArray& data) { ... };
    context.seedDir  = [&](const QString& path) { ... };
    context.expectsWriteSupport = false;   // read-only backend

    mole::test::runFileSystemConformance(context);
}
```

### Making a file mountable

To make activating a file open it as a drive:

```cpp
QStringList mountableFileSuffixes() const override { return { QStringLiteral("iso") }; }
QVariantMap configForFile(const QString& path) const override { ... }
mole::VfsUri rootUriForFile(const QString& path) const override { ... }
// The other direction, and it is not optional in practice:
QVariantMap configForRoot(const mole::VfsUri& root) const override { ... }
```

The host asks every factory; it never hardcodes a list of file types.

**`configForRoot()` is the one that is easy to leave out and expensive to.** It
is `rootUriForFile()` backwards: given a root uri, say what config would produce
it. Without it a bookmark to a mounted file, or a session restored onto one,
arrives at a uri with nothing behind it — the mount is gone and only the address
is left. See
[ADR-0083](adr/0083-a-mount-can-be-a-place-without-being-a-drive.md) and MOLE-310.

## Adding a tab

Implement `IFeature` (the factory) and subclass `FeatureController` (the
per-tab state). One controller is created per opened tab, so ten tabs of your
feature are ten independent instances.

```cpp
class DuplicateController final : public mole::FeatureController {
    Q_OBJECT
    // Your own model, and it has to be: the shell's FileListModel lives in
    // src/ui/models, which is a layer above the SDK -- a plugin cannot see it,
    // and a sample that used it was teaching the layering being broken.
    Q_PROPERTY(QAbstractItemModel* results READ results CONSTANT)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
public:
    explicit DuplicateController(const mole::PluginServices& services, QObject* parent)
        : FeatureController(QStringLiteral("Duplicates"), parent)
        , m_services(services) {}

    Q_INVOKABLE void start()
    {
        auto* task = new FindDuplicatesTask(...);
        connect(task, &Task::finished, this, [this, task] { ... });
        m_services.tasks->submit(task);   // never do the work here
    }
};

class DuplicateFeature final : public mole::IFeature {
public:
    QString id() const override { return QStringLiteral("org.example.duplicates"); }
    QString title() const override { return QStringLiteral("Duplicates"); }
    QString description() const override { return QStringLiteral("Find identical files."); }
    QString iconText() const override { return QStringLiteral("⧉"); }

    // The four with defaults, and what they are for:
    int sortOrder() const override { return 100; }        // where it sits among tabs
    bool opensFromNothing() const override { return true; }  // see the note below
    bool needsContext() const override { return false; }  // true: wants a folder to work on
    QStringList absorbedIds() const override { return {}; }  // ids this feature replaced

    QUrl viewSource() const override
    {
        return QUrl(QStringLiteral("qrc:/org/example/duplicates/DuplicatesView.qml"));
    }

    mole::FeatureController* createController(QObject* parent) override
    {
        return new DuplicateController(m_services, parent);
    }
};
```

**`opensFromNothing()` decides whether the shell offers your tab by itself, and
answering `false` puts an obligation on you.** A feature that needs to be given
something — a folder, a selection — is not offered from an empty window, so it
must register a `MenuAction` whose `opensFeature` names its id, or there is no way
in at all. `tst_AppIntegration::everyFeatureIsReachableFromTheMenu` fails when
one is missing, which is where you will meet this rule if you skip it. See
[ADR-0032](adr/0032-a-feature-says-whether-a-new-tab-of-it-means-anything.md).

`absorbedIds()` is how a feature takes over a saved tab that used to be another
one: a session holding the old id opens yours instead of nothing.

A controller can also say where it is looking with `openLocations()`, which is
what puts a tab's folder in front of anything that asks the window what is on
screen.

Your QML receives the controller as a `controller` property:

```qml
Item {
    property var controller: null
    Button { text: "Scan"; onClicked: controller.start() }
    ListView { model: controller ? controller.results : null }
}
```

Ship the QML inside your plugin's own `qt_add_qml_module` or `.qrc`. Use
`setTitle()` to rename the tab as its state changes; the shell follows along
without knowing what your feature does.

### Remembering the tab across restarts

```cpp
QVariantMap saveState() const override
{
    return { { QStringLiteral("folder"), m_folder },
             { QStringLiteral("mode"), m_mode } };
}

void restoreState(const QVariantMap& state) override
{
    // Treat every value as untrusted: it may come from an older release or a
    // hand-edited file.
    m_folder = state.value(QStringLiteral("folder")).toString();
    m_mode = state.value(QStringLiteral("mode"), 0).toInt();
}
```

Emit `stateChanged()` whenever something worth persisting moves. The shell
debounces it, so emitting often is fine. Save the criteria, not the results:
re-running expensive work at startup is a surprise, and showing stale results
is worse.

## Adding a preview

```cpp
class ParquetPreviewProvider final : public mole::IPreviewProvider {
public:
    QString id() const override { return QStringLiteral("org.example.parquet"); }
    QString displayName() const override { return QStringLiteral("Parquet"); }
    int priority() const override { return 50; }   // outranks the text fallback

    bool canPreview(const mole::FileEntry& entry) const override
    {
        return entry.uri.suffix() == QLatin1String("parquet");   // no I/O here
    }

    QUrl viewSource() const override { return QUrl(QStringLiteral("qrc:/.../View.qml")); }
    mole::PreviewController* createController(QObject* parent) override { ... }
};
```

Highest priority wins, so you can override a built-in viewer by outranking it.
`canPreview()` must be cheap — name, suffix and size only.

Read file content through `ReadFileTask` rather than opening the file yourself;
it works on every backend and caps how much it pulls, which matters when the
file is a 2 GB log over SFTP.

**`decline()` is the one call a viewer needs and the one most likely to be
missed.** `canPreview()` answers from a name, so a viewer regularly opens a file
it then cannot show: a JPEG that is not a JPEG, a database whose header lies, a
PDF that is thirty bytes. Emitting `decline(reason)` from the controller hands the
file to the next provider below yours in the registry's order, and the reason is
shown if nothing else claims it. A viewer that instead shows an empty pane, or
asserts, is a viewer that takes the window with it. See
[ADR-0078](adr/0078-a-viewer-may-decline-a-file-it-has-read.md).

**Options are a viewer's own settings, and they persist.** Answer `options()`
with the choices your viewer offers for a file — word wrap, an encoding, a page
fit — and the shell draws them and remembers what was chosen, per file type,
handing it back through `setViewerOption()`. Nothing is stored by your plugin. See
[ADR-0006](adr/0006-preview-options-and-preferences.md).

## Adding a metadata reader

Every reader that claims a file contributes, unlike a preview provider where one
wins:

```cpp
class ExifReader final : public mole::IMetadataReader {
public:
    QString id() const override { return QStringLiteral("org.example.exif"); }
    bool claims(const mole::FileEntry& entry) const override { ... }   // cheap, no I/O

    QList<mole::FileFact> read(const mole::FileEntry& entry, QByteArrayView head,
        const mole::PluginServices& services, const mole::CancelToken& cancel) const override
    {
        // `head` is the first few kilobytes, already read. Poll `cancel`.
        return { mole::FileFact { ... } };
    }
};
```

`services` is a `const&`, and that is an ABI decision rather than a style one —
see [ADR-0098](adr/0098-the-plugin-api-version-is-checked-before-the-plugin-runs.md).
Returning nothing is ordinary: a reader that claimed a file and found no tags in
it costs its own rows and nobody else's.

## Adding a thumbnailer

The opposite rule to readers: only the highest-priority claimant is asked,
because a file has one picture.

```cpp
class SvgThumbnailer final : public mole::IThumbnailer {
public:
    QString id() const override { return QStringLiteral("org.example.svg"); }
    int priority() const override { return 50; }
    bool claims(const mole::FileEntry& entry) const override { ... }

    QImage thumbnail(const mole::FileEntry& entry, int size,
        const mole::PluginServices& services, const mole::CancelToken& cancel) const override
    {
        // A null image means "no thumbnail" and is an ordinary answer.
        // `cancel` must be polled: a folder scrolled past leaves its decodes
        // pointless.
        return {};
    }
};
```

See [ADR-0058](adr/0058-a-file-can-say-what-it-looks-like.md).

## The two through services()

Not everything a plugin can contribute goes through `PluginRegistry`. Two are
registered on objects reached from `registry.services()`:

- **A chain step kind**, on `services().chains`. A step kind is an operation a
  chain can be built from; the chains that use it outlive the plugin, which is
  why a kind may be replaced rather than only added. See
  [ADR-0082](adr/0082-a-chain-is-a-line-and-a-list-of-uris-passes-along-it.md).
- **A scheduled job kind**, on `services().scheduler`. Register a kind to have
  the host run it on a schedule; the rules themselves outlive the plugin that
  made them.

Both are worth knowing about before deciding your feature needs a tab: an
operation offered as a step and as a scheduled job is available from the chain
editor, the scheduler and a menu entry without a window of its own.

## Adding a menu entry

Anything a plugin contributes needs a way in, and for a feature that answers
`false` to `opensFromNothing()` this *is* the way in. The sections are `File`,
`View`, `Operations`, `Workflows` and `Bookmarks` — there is no `Tools`, which is
what this said for long after the two halves of it were renamed. `Operations`
acts on the selection, or on the current folder when nothing is selected, and
leaves you where you were; `Workflows` opens a tab that is a tool you then work
in; `View` is for how the current tab looks.

```cpp
void registerExtensions(mole::PluginRegistry& registry) override
{
    mole::MenuAction action;
    action.id = QStringLiteral("org.example.duplicates.scan");  // namespaced
    // Workflows opens a tool in a tab of its own; Operations does something to
    // the files in front of you and leaves you where you were. One question
    // decides it, and ADR-0003 works through the cases that sound like both.
    action.section = mole::MenuAction::Section::Workflows;
    action.title = QStringLiteral("Find duplicates here");
    action.iconText = QStringLiteral("⧉");
    action.sortOrder = 50;               // built-ins leave gaps; 500 is the default
    action.trigger = [services = registry.services()] { /* queue a Task */ };
    action.enabled = [] { return true; };   // optional; greys out rather than hides
    registry.addMenuAction(std::move(action));
}
```

**`enabled` and `checked` are asked again every time the menu opens**, so an
entry can reflect the current tab without anyone having to invalidate anything.
`trigger` is not re-evaluated — it is called when the entry is chosen, which is
the only time it means anything. Give `checked` a value and the entry becomes a
tick box.

Set `opensFeature` to a feature id instead of a `trigger` to have the entry open
that tab; that is the pairing `opensFromNothing()` requires above.

Menu action ids share a namespace with each other, and a clash is refused and
logged with your plugin's name in the message. The same is true of plugin ids,
feature ids, preview ids and drive schemes.

## Reacting to events

`registry.services().events` is the application-wide bus.

```cpp
connect(services.events, &mole::EventBus::directoryChanged, this,
        [this](const mole::VfsUri& dir) { if (dir == m_current) refresh(); });
```

Publish with `postDirectoryChanged()`, `postIndexUpdated()`,
`postNotification()`, or `postCustom("org.example.gitlab/branchChanged", payload)`
for your own events. All of them are safe to call from a worker thread and
always deliver on the UI thread.

**A custom topic is not checked against anything.** `postCustom()` takes the
string and emits it; nothing registers a topic and nothing refuses a duplicate,
so two plugins choosing the same string quietly hear each other's events. Namespace
it with your plugin id, as above — that is the whole of the protection, and this
page used to claim there was more.

## Rules that will bite you

1. **Never block the UI thread.** Submit a `Task`. Everything else follows from
   this one.
2. **Namespace your ids.** Plugin id, feature id, preview id, thumbnailer id,
   reader id, menu action id and drive scheme each share a namespace with their
   own kind, and a clash is refused and logged with your plugin's name in the
   message. **Custom event topics are the exception**: nothing checks those, so
   namespacing one is on you.
3. **Never change a released id.** Open tabs are persisted by feature id.
4. **`apiVersion` must match the host, and the version is in the interface
   identifier.** Leave the default and rebuild against the SDK you target. The
   refusal happens twice: `MOLE_PLUGIN_IID` carries the version, so a library
   built against another one is refused from its Qt metadata — before its
   constructor runs, let alone before it is asked what it is — and
   `PluginMetadata::apiVersion` is checked again on the object. The first of
   those is what makes this a promise rather than a hope: until MOLE-366 the host
   read the version *by calling into the plugin*, which is asking a question in a
   shape the plugin may not have. See
   [ADR-0098](adr/0098-the-plugin-api-version-is-checked-before-the-plugin-runs.md).
5. **Errors, not exceptions.** Return `Result<T>` with a `VfsError`.
6. **Registration is not initialisation.** `registerExtensions()` should be
   nearly instant; startup waits for it.
