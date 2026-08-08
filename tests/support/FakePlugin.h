#pragma once

#include "sdk/FeatureController.h"
#include "sdk/PluginApi.h"

#include "core/vfs/backends/MemoryFileSystem.h"

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
