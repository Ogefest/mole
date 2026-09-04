#pragma once

#include "sdk/FeatureController.h"
#include "sdk/PluginApi.h"

#include "core/automation/Chain.h"
#include "core/vfs/backends/MemoryFileSystem.h"

#include <QColor>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <memory>
#include <stdexcept>

namespace mole::test {

/// A feature that exists only to be registered and counted.
class FakeFeature final : public IFeature
{
public:
    FakeFeature(QString id, QString title)
        : m_id(std::move(id))
        , m_title(std::move(title))
    {
    }

    QString id() const override { return m_id; }
    QString title() const override { return m_title; }
    QString description() const override { return QStringLiteral("fake"); }
    QString iconText() const override { return QStringLiteral("*"); }
    QUrl viewSource() const override { return QUrl(QStringLiteral("qrc:/fake/View.qml")); }

    FeatureController* createController(QObject* parent) override
    {
        ++m_created;
        return new FeatureController(m_title, parent);
    }

    int createdCount() const { return m_created; }

private:
    QString m_id;
    QString m_title;
    int m_created = 0;
};

class FakePreviewProvider final : public IPreviewProvider
{
public:
    FakePreviewProvider(QString id, QString suffix, int priority)
        : m_id(std::move(id))
        , m_suffix(std::move(suffix))
        , m_priority(priority)
    {
    }

    QString id() const override { return m_id; }
    QString displayName() const override { return m_id; }
    int priority() const override { return m_priority; }

    bool canPreview(const FileEntry& entry) const override
    {
        return !entry.isDir && (m_suffix.isEmpty() || entry.uri.suffix() == m_suffix);
    }

    QUrl viewSource() const override { return QUrl(QStringLiteral("qrc:/fake/Preview.qml")); }
    PreviewController* createController(QObject*) override { return nullptr; }

private:
    QString m_id;
    QString m_suffix;
    int m_priority = 0;
};

/// A reader that says whatever the test told it to, and records that it ran.
///
/// The counters are shared rather than owned, because the registry takes the
/// reader and the test still has to see what happened to it -- and because a
/// reader runs on a worker thread, they are atomic.
class FakeMetadataReader final : public IMetadataReader
{
public:
    struct Log
    {
        std::atomic_int reads { 0 };
        std::atomic_int cancelled { 0 };
        /// Bytes of head the reader was handed the last time it ran.
        std::atomic<qsizetype> headSize { 0 };
        /// Whether the last read was handed live services. Every shipped reader
        /// that needs bytes past the sniff page -- EXIF further into a JPEG, a
        /// docx whose core.xml is not at the front, tags at the end of an audio
        /// file -- checks `services.vfs` and gives up without one, so a caller
        /// that passes an empty struct silently gets nothing from any of them.
        std::atomic_bool sawServices { false };
    };

    FakeMetadataReader(QString id, QList<FileFact> facts, int priority = 0,
        std::shared_ptr<Log> log = nullptr, QString suffix = {})
        : m_id(std::move(id))
        , m_facts(std::move(facts))
        , m_priority(priority)
        , m_log(std::move(log))
        , m_suffix(std::move(suffix))
    {
    }

    /// Waits for this before answering, so a test can hold a reader still and
    /// step to another file underneath it. Not a sleep: released by the test.
    void holdUntilReleased(std::shared_ptr<QSemaphore> gate) { m_gate = std::move(gate); }
    /// Throws instead of answering, which one reader in a panel is allowed to do
    /// without costing the others their rows.
    void failInstead() { m_fails = true; }

    QString id() const override { return m_id; }
    int priority() const override { return m_priority; }

    bool canRead(const FileEntry& entry) const override
    {
        return !entry.isDir && (m_suffix.isEmpty() || entry.uri.suffix() == m_suffix);
    }

    QList<FileFact> read(const FileEntry& entry, QByteArrayView head, const PluginServices& services,
        const CancelToken& cancel) const override
    {
        Q_UNUSED(entry);
        if (m_log) {
            ++m_log->reads;
            m_log->headSize = head.size();
            m_log->sawServices = services.vfs != nullptr;
        }
        if (m_gate)
            m_gate->acquire();
        if (cancel.isCancelled() && m_log)
            ++m_log->cancelled;
        if (m_fails)
            throw std::runtime_error("this reader is having a bad afternoon");
        return m_facts;
    }

private:
    QString m_id;
    QList<FileFact> m_facts;
    int m_priority = 0;
    std::shared_ptr<Log> m_log;
    QString m_suffix;
    std::shared_ptr<QSemaphore> m_gate;
    bool m_fails = false;
};

/// A thumbnailer that answers with a flat colour, counts what it was asked and can
/// be held still.
///
/// The counters are shared rather than owned, for the same reason the metadata
/// reader's are: the registry takes the thumbnailer and the test still has to see
/// what happened to it, and it runs on a worker thread.
class FakeThumbnailer final : public IThumbnailer
{
public:
    struct Log
    {
        std::atomic_int made { 0 };
        /// How many were asked for and saw their cancel token already set.
        std::atomic_int cancelled { 0 };
        /// The size the last request asked for, so a test can hold that the tile's
        /// own size reaches the thumbnailer.
        std::atomic_int lastSize { 0 };
        /// The thread the last one ran on, for the rule that nothing decodes on
        /// the GUI thread.
        std::atomic<QThread*> ranOn { nullptr };
    };

    FakeThumbnailer(
        QString id, QColor colour, int priority = 0, std::shared_ptr<Log> log = nullptr, QString suffix = {})
        : m_id(std::move(id))
        , m_colour(colour)
        , m_priority(priority)
        , m_log(std::move(log))
        , m_suffix(std::move(suffix))
    {
    }

    /// Waits for this before answering, so a test can hold a decode still and
    /// cancel it underneath. Not a sleep: released by the test.
    void holdUntilReleased(std::shared_ptr<QSemaphore> gate) { m_gate = std::move(gate); }
    /// Answers with nothing, which is what an undecodable file looks like.
    void answerWithNothing() { m_empty = true; }

    QString id() const override { return m_id; }
    int priority() const override { return m_priority; }

    bool canThumbnail(const FileEntry& entry) const override
    {
        return !entry.isDir && (m_suffix.isEmpty() || entry.uri.suffix() == m_suffix);
    }

    QImage thumbnail(const FileEntry& entry, int size, const PluginServices& services,
        const CancelToken& cancel) const override
    {
        Q_UNUSED(entry);
        Q_UNUSED(services);
        if (m_log) {
            ++m_log->made;
            m_log->lastSize = size;
            m_log->ranOn = QThread::currentThread();
        }
        if (m_gate)
            m_gate->acquire();
        if (cancel.isCancelled()) {
            if (m_log)
                ++m_log->cancelled;
            return {};
        }
        if (m_empty)
            return {};

        QImage image(size, size, QImage::Format_ARGB32);
        image.fill(m_colour);
        return image;
    }

private:
    QString m_id;
    QColor m_colour;
    int m_priority = 0;
    std::shared_ptr<Log> m_log;
    QString m_suffix;
    std::shared_ptr<QSemaphore> m_gate;
    bool m_empty = false;
};

/// Lets go of a held reader's gate on the way out of a test, however it leaves.
///
/// A reader waiting on a gate is a blocked worker thread, and the fixture's
/// teardown waits for it. A `release()` written at the bottom of the test is not
/// reached when a `QVERIFY` above it fails -- QTest returns from the function --
/// so the thread never comes back, teardown waits for it, and ctest kills the
/// whole binary at its timeout. What the log then says is `Timeout`, with no
/// mention of the assertion that actually failed. One of these beside the gate
/// makes that impossible, and it costs nothing when everything passes: the
/// permits nobody is waiting on are never taken.
///
/// See MOLE-217, which was three minutes of a suite run and a lost diagnosis.
class GateRelease
{
public:
    explicit GateRelease(std::shared_ptr<QSemaphore> gate, int permits = 64)
        : m_gate(std::move(gate))
        , m_permits(permits)
    {
    }
    ~GateRelease()
    {
        if (m_gate)
            m_gate->release(m_permits);
    }

    GateRelease(const GateRelease&) = delete;
    GateRelease& operator=(const GateRelease&) = delete;

private:
    std::shared_ptr<QSemaphore> m_gate;
    int m_permits;
};

/// A filesystem factory for a scheme nobody else claims.
class FakeFileSystemFactory final : public IFileSystemFactory
{
public:
    explicit FakeFileSystemFactory(QString scheme)
        : m_scheme(std::move(scheme))
    {
    }

    QString scheme() const override { return m_scheme; }
    QString displayName() const override { return m_scheme; }
    QList<ConnectionField> connectionFields() const override { return m_fields; }
    FileSystemPtr create(const QVariantMap&, QString*) override
    {
        return std::make_shared<MemoryFileSystem>();
    }

    /// The three answers a factory can give about itself, so what the drive list
    /// does with each is testable without a real backend or a missing library.
    bool isAvailable() const override { return m_available; }
    QString unavailableReason() const override { return m_unavailableReason; }
    bool isApplicable() const override { return m_applicable; }

    void setConnectionFields(QList<ConnectionField> fields) { m_fields = std::move(fields); }
    void setUnavailable(QString reason)
    {
        m_available = false;
        m_unavailableReason = std::move(reason);
    }
    /// Not a kind of drive on this platform at all -- what SMB and NFS are on
    /// Windows, where a share is reached by the local filesystem.
    void setNotApplicable() { m_applicable = false; }

private:
    QString m_scheme;
    QList<ConnectionField> m_fields;
    bool m_available = true;
    bool m_applicable = true;
    QString m_unavailableReason;
};

/// Configurable plugin used to drive PluginManager through its edge cases.
/// A chain step kind that exists only to be registered and asked about itself.
class FakeChainStepKind final : public IChainStepKind
{
public:
    FakeChainStepKind(QString id, StepRole role)
        : m_id(std::move(id))
        , m_role(role)
    {
    }

    QString kind() const override { return m_id; }
    QString displayName() const override { return m_id; }
    StepRole role() const override { return m_role; }
    QList<StepParameter> parameters() const override
    {
        StepParameter where;
        where.key = QStringLiteral("where");
        where.label = QStringLiteral("Where");
        where.kind = StepParameter::Kind::Uri;
        where.required = true;
        return { where };
    }

    /// Enough to be run as well as described: it hands back what it was given,
    /// which is what a step that does nothing looks like from the chain's side.
    StepOutcome run(const ChainStep&, const QStringList& incoming, const StepContext&) override
    {
        return StepOutcome::produced(incoming);
    }

private:
    QString m_id;
    StepRole m_role;
};

class FakePlugin final : public IPlugin
{
public:
    struct Config
    {
        QString id = QStringLiteral("test.fake");
        int apiVersion = kPluginApiVersion;
        QStringList featureIds;
        QStringList schemes;
        QStringList previewIds;
        QStringList metadataReaderIds;
        QStringList thumbnailerIds;
        QStringList menuActionIds;
        /// Step kinds this plugin contributes, as `id` -- registered through
        /// PluginServices the way a real one would, so a test can assert that a
        /// plugin's step is offered exactly like a built-in.
        QList<QPair<QString, StepRole>> chainStepKinds;
        /// Set to true by shutdown(). Owned by the test, because the manager
        /// destroys the plugin before the test can inspect it.
        bool* shutdownFlag = nullptr;
        /// Throws from registerExtensions(), which is plugin code the host runs
        /// and therefore plugin code that can throw. Nothing in the loader used
        /// to catch it, so one bad plugin ended the process at startup.
        bool throwOnRegister = false;
        /// The same for metadata(), which the loader calls first.
        bool throwOnMetadata = false;
    };

    explicit FakePlugin(Config config)
        : m_config(std::move(config))
    {
    }

    PluginMetadata metadata() const override
    {
        if (m_config.throwOnMetadata)
            throw std::runtime_error("this plugin cannot say what it is");
        PluginMetadata data;
        data.id = m_config.id;
        data.name = m_config.id;
        data.version = QStringLiteral("1.0");
        data.apiVersion = m_config.apiVersion;
        return data;
    }

    void registerExtensions(PluginRegistry& registry) override
    {
        m_sawServices = registry.services().isValid();
        if (m_config.throwOnRegister)
            throw std::runtime_error("this plugin fell over while registering");
        for (const QString& id : m_config.featureIds)
            registry.addFeature(std::make_unique<FakeFeature>(id, id));
        for (const QString& scheme : m_config.schemes)
            registry.addFileSystemFactory(std::make_unique<FakeFileSystemFactory>(scheme));
        for (const QString& id : m_config.previewIds)
            registry.addPreviewProvider(std::make_unique<FakePreviewProvider>(id, QString(), 0));
        for (const QString& id : m_config.metadataReaderIds)
            registry.addMetadataReader(std::make_unique<FakeMetadataReader>(id, QList<FileFact> {}));
        for (const QString& id : m_config.thumbnailerIds)
            registry.addThumbnailer(std::make_unique<FakeThumbnailer>(id, QColor(Qt::red)));
        for (const auto& [id, role] : m_config.chainStepKinds) {
            if (!registry.services().chains)
                continue;
            m_stepKinds.push_back(std::make_unique<FakeChainStepKind>(id, role));
            registry.services().chains->registerKind(m_stepKinds.back().get());
        }
        for (const QString& id : m_config.menuActionIds) {
            MenuAction action;
            action.id = id;
            action.section = MenuAction::Section::Workflows;
            action.title = id;
            action.trigger = [] {};
            registry.addMenuAction(std::move(action));
        }
    }

    void shutdown() override
    {
        if (m_config.shutdownFlag)
            *m_config.shutdownFlag = true;
    }

    bool sawValidServices() const { return m_sawServices; }

private:
    std::vector<std::unique_ptr<FakeChainStepKind>> m_stepKinds;
    Config m_config;
    bool m_sawServices = false;
};

} // namespace mole::test
