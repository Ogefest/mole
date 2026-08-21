#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "sdk/PluginServices.h"

#include "core/rename/RenamePlan.h"

#include <QPointer>
#include <QVariantList>

namespace mole {

class Task;

/// Renaming a batch, with the result visible before anything happens.
///
/// The preview is the feature. Renaming two hundred files on faith is how people
/// lose an evening -- and a batch that half-succeeds is worse than one that never
/// ran, which is why a plan that would collide cannot be applied at all.
class BulkRenameController final : public FeatureController
{
    Q_OBJECT
    /// The files being renamed. Taken from wherever the tab was opened -- a
    /// pane's selection or a set -- and then fixed, so the list cannot shift
    /// underneath a preview the user is reading.
    Q_PROPERTY(QStringList sources READ sourceUris NOTIFY sourcesChanged)
    Q_PROPERTY(int sourceCount READ sourceCount NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList rules READ rules NOTIFY rulesChanged)
    /// Before and after for every file, recomputed on every edit.
    Q_PROPERTY(QVariantList preview READ preview NOTIFY previewChanged)
    Q_PROPERTY(int changedCount READ changedCount NOTIFY previewChanged)
    Q_PROPERTY(int blockedCount READ blockedCount NOTIFY previewChanged)
    Q_PROPERTY(bool canApply READ canApply NOTIFY previewChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY previewChanged)
    Q_PROPERTY(QVariantList ruleKinds READ ruleKinds CONSTANT)

public:
    BulkRenameController(PluginServices services, QObject* parent = nullptr);
    ~BulkRenameController() override;

    QStringList sourceUris() const;
    int sourceCount() const { return static_cast<int>(m_sources.size()); }
    /// Written by the shell when the tab opens. Named to match what every other
    /// operation takes.
    Q_INVOKABLE void setTargets(const QStringList& uris);

    QVariantList rules() const;
    QVariantList ruleKinds() const;
    QVariantList preview() const;
    int changedCount() const { return m_plan.changedCount(); }
    int blockedCount() const { return m_plan.blockedCount(); }
    bool canApply() const;
    QString summary() const;

    Q_INVOKABLE void addRule(const QString& kind);
    Q_INVOKABLE void removeRule(int index);
    Q_INVOKABLE void moveRule(int index, int delta);
    Q_INVOKABLE void setRuleEnabled(int index, bool enabled);
    /// Updates one field of one rule. Field names match RenameRule's members,
    /// so the form does not need a setter per rule kind.
    Q_INVOKABLE void setRuleField(int index, const QString& field, const QVariant& value);

    Q_INVOKABLE void apply();

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void sourcesChanged();
    void rulesChanged();
    void previewChanged();

private:
    void rebuildPreview();
    void refreshDirectoryContents();

    PluginServices m_services;
    QList<VfsUri> m_sources;
    QList<RenameRule> m_rules;
    RenamePlan m_plan;
    /// What is already in each directory involved, so a rename onto an existing
    /// name is caught in the preview rather than by the filesystem.
    QHash<QString, QStringList> m_existing;
    /// What the drive underneath does about case, read when the targets are set.
    Qt::CaseSensitivity m_caseSensitivity = Qt::CaseSensitive;
    QPointer<Task> m_task;
};

class BulkRenameFeature final : public IFeature
{
public:
    explicit BulkRenameFeature(PluginServices services);

    QString id() const override { return QStringLiteral("core.bulkrename"); }
    QString title() const override { return QStringLiteral("Bulk rename"); }
    QString description() const override
    {
        return QStringLiteral("Rename many files at once, with a preview first");
    }
    QString iconText() const override { return QStringLiteral("Aa"); }
    int sortOrder() const override { return 50; }
    /// Meaningless without files to rename.
    bool needsContext() const override { return true; }
    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

} // namespace mole
