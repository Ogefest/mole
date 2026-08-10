#pragma once

#include "sdk/FeatureController.h"
#include "sdk/PluginApi.h"

#include "core/vfs/backends/MemoryFileSystem.h"

#include <QSemaphore>

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

    QList<FileFact> read(const FileEntry& entry, QByteArrayView head, PluginServices services,
        const CancelToken& cancel) const override
    {
        Q_UNUSED(entry);
        Q_UNUSED(services);
        if (m_log) {
            ++m_log->reads;
            m_log->headSize = head.size();
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
    FileSystemPtr create(const QVariantMap&, QString*) override
    {
        return std::make_shared<MemoryFileSystem>();
    }

private:
    QString m_scheme;
};

/// Configurable plugin used to drive PluginManager through its edge cases.
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
        QStringList menuActionIds;
        /// Set to true by shutdown(). Owned by the test, because the manager
        /// destroys the plugin before the test can inspect it.
        bool* shutdownFlag = nullptr;
    };

    explicit FakePlugin(Config config)
        : m_config(std::move(config))
    {
    }

    PluginMetadata metadata() const override
    {
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
        for (const QString& id : m_config.featureIds)
            registry.addFeature(std::make_unique<FakeFeature>(id, id));
        for (const QString& scheme : m_config.schemes)
            registry.addFileSystemFactory(std::make_unique<FakeFileSystemFactory>(scheme));
        for (const QString& id : m_config.previewIds)
            registry.addPreviewProvider(std::make_unique<FakePreviewProvider>(id, QString(), 0));
        for (const QString& id : m_config.metadataReaderIds)
            registry.addMetadataReader(std::make_unique<FakeMetadataReader>(id, QList<FileFact> {}));
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
    Config m_config;
    bool m_sawServices = false;
};

} // namespace mole::test
