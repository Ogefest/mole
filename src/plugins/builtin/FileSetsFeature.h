#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "sdk/PluginServices.h"

#include "core/sets/FileSetStore.h"

#include <QPointer>
#include <QVariantList>

namespace mole {

class ListDirectoryTask;

/// A tab showing one set: what is in it, and what can be done to it.
///
/// The important part is `targets()`. Every operation in the application already
/// takes "the things to act on" as a list of uris, which is what a pane's
/// selection provides. A set provides exactly the same thing under exactly the
/// same name, so copying a set, reporting on it or renaming its members needs no
/// new code in any of those operations -- which is the difference between adding
/// a feature and adding a second code path to every feature there is.
class FileSetsController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(QVariantList sets READ sets NOTIFY setsChanged)
    Q_PROPERTY(QString currentSetId READ currentSetId WRITE setCurrentSetId NOTIFY currentChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentChanged)
    /// The members, with whatever is known about each one.
    Q_PROPERTY(QVariantList members READ members NOTIFY membersChanged)
    Q_PROPERTY(int memberCount READ memberCount NOTIFY membersChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY membersChanged)
    /// Members whose file is no longer there. A set outlives the things in it,
    /// and pretending otherwise is how an operation half-fails.
    Q_PROPERTY(int missingCount READ missingCount NOTIFY membersChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)

public:
    FileSetsController(PluginServices services, FileSetStore* store, QObject* parent = nullptr);
    ~FileSetsController() override;

    QVariantList sets() const;
    QString currentSetId() const { return m_currentId; }
    void setCurrentSetId(const QString& id);
    QString currentName() const;
    QVariantList members() const;
    int memberCount() const;
    QString summary() const;
    int missingCount() const;
    QString filter() const { return m_filter; }
    void setFilter(const QString& filter);

    /// What an operation should act on. The same shape a pane's selection has,
    /// deliberately and by the same name.
    QList<VfsUri> targets() const;
    Q_INVOKABLE QStringList targetUris() const;
    Q_INVOKABLE int targetCount() const { return memberCount(); }

    Q_INVOKABLE QString createSet(const QString& name);
    Q_INVOKABLE bool renameSet(const QString& id, const QString& name);
    Q_INVOKABLE bool removeSet(const QString& id);
    Q_INVOKABLE int addUris(const QStringList& uris);
    Q_INVOKABLE int removeUris(const QStringList& uris);
    /// Drops every member whose file has gone.
    Q_INVOKABLE int forgetMissing();
    /// Re-checks each member against its drive.
    Q_INVOKABLE void verify();
    /// Says out loud that a member's file has gone.
    ///
    /// The view already knows -- each member carries `missing` -- and has no
    /// voice of its own; this has the event bus. Called when a key declines to
    /// act on a member, because a key that does nothing cannot be told apart
    /// from a key that is broken. See MOLE-205.
    Q_INVOKABLE void reportMissing(const QString& uri);

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void setsChanged();
    void currentChanged();
    void membersChanged();
    void filterChanged();

private:
    void refresh();

    PluginServices m_services;
    FileSetStore* m_store = nullptr;
    QString m_currentId;
    QString m_filter;

    /// What each member turned out to be, from the last verify. Absent means
    /// "not checked yet", which is shown differently from "not there".
    QHash<QString, bool> m_present;
    QHash<QString, qint64> m_sizes;
};

class FileSetsFeature final : public IFeature
{
public:
    FileSetsFeature(PluginServices services, FileSetStore* store);

    QString id() const override { return QStringLiteral("core.filesets"); }
    QString title() const override { return QStringLiteral("Sets"); }
    QString description() const override
    {
        return QStringLiteral("A named list of files to work on as one thing");
    }
    QString iconText() const override { return QStringLiteral("☷"); }
    int sortOrder() const override { return 48; }
    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
    FileSetStore* m_store = nullptr;
};

} // namespace mole
