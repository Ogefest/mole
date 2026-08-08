# Writing a plugin

A plugin is a shared library that implements `mole::IPlugin`. It can contribute
four things: drives, tabs, previews and menu entries. Nothing else in the host
is reachable, which is what makes the API a contract rather than an invitation
to poke around.

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

```cmake
qt_add_plugin(gitlab_plugin SHARED CLASS_NAME GitLabPlugin)
# Qt 6.4's qt_add_plugin() ignores trailing source arguments.
target_sources(gitlab_plugin PRIVATE GitLabPlugin.cpp GitLabFileSystem.cpp)
target_link_libraries(gitlab_plugin PRIVATE mole_sdk)
set_target_properties(gitlab_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
```

The host looks in `<executable dir>/plugins`, in the user's data directory, and
in anything listed in `MOLE_PLUGIN_PATH`.

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
```

The host asks every factory; it never hardcodes a list of file types.

## Adding a tab

Implement `IFeature` (the factory) and subclass `FeatureController` (the
per-tab state). One controller is created per opened tab, so ten tabs of your
feature are ten independent instances.

```cpp
class DuplicateController final : public mole::FeatureController {
    Q_OBJECT
    Q_PROPERTY(mole::FileListModel* results READ results CONSTANT)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
public:
    explicit DuplicateController(mole::PluginServices services, QObject* parent)
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

## Adding a menu entry

Anything a plugin contributes needs a way in. `Tools` is where operations on
files belong; `View` is for how the current tab looks.

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

`trigger`, `enabled` and `checked` are re-evaluated every time the menu opens,
so an entry can reflect the current tab without anyone having to invalidate
anything. Give `checked` a value and the entry becomes a tick box.

Ids share a global namespace: a clash is refused and logged with your plugin's
name in the message.

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

## Rules that will bite you

1. **Never block the UI thread.** Submit a `Task`. Everything else follows from
   this one.
2. **Namespace your ids.** Plugin id, feature id, preview id and custom event
   topics all share a global namespace; a clash is refused and logged with your
   plugin's name in the message.
3. **Never change a released id.** Open tabs are persisted by feature id.
4. **`apiVersion` must match the host.** Leave the default and rebuild against
   the SDK you target; a mismatch is refused at load with a clear message
   instead of crashing later.
5. **Errors, not exceptions.** Return `Result<T>` with a `VfsError`.
6. **Registration is not initialisation.** `registerExtensions()` should be
   nearly instant; startup waits for it.
