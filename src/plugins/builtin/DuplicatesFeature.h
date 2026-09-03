#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "sdk/PluginServices.h"
#include "ui/models/DuplicateGroupModel.h"

#include "core/duplicates/FindDuplicatesTask.h"

#include <QPointer>
#include <QVariantList>

namespace mole {

/// Finding duplicates, and deciding what to do about them.
///
/// Choosing what to keep is the hard half. The tab never picks for the user, but
/// it offers the choices people actually make -- keep the oldest, keep the one
/// in a particular folder -- and shows what each would free before anything is
/// deleted.
class DuplicatesController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(QStringList roots READ roots NOTIFY rootsChanged)
    Q_PROPERTY(QVariantList strategies READ strategies CONSTANT)
    Q_PROPERTY(QString strategyId READ strategyId WRITE setStrategyId NOTIFY optionsChanged)
    Q_PROPERTY(QString strategyDescription READ strategyDescription NOTIFY optionsChanged)
    Q_PROPERTY(qint64 minimumSize READ minimumSize WRITE setMinimumSize NOTIFY optionsChanged)
    /// The confirmed groups, as a model. Constant because the object never
    /// changes -- what changes is its contents, and it says so itself, one row at
    /// a time. A list property re-read on every confirmation is the fault
    /// MOLE-210 fixed; see DuplicateGroupModel.
    Q_PROPERTY(mole::DuplicateGroupModel* groups READ groups CONSTANT)
    Q_PROPERTY(int groupCount READ groupCount NOTIFY resultsChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY resultsChanged)
    Q_PROPERTY(bool scanning READ isScanning NOTIFY resultsChanged)
    Q_PROPERTY(bool hasRun READ hasRun NOTIFY resultsChanged)
    /// What the scan is doing right now -- which stage, over how many files.
    /// Empty when nothing is running.
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
    /// Whether the last scan was stopped rather than finished. What it found is
    /// kept; saying it completed would be a lie, and the difference matters
    /// because a stopped scan may have been about to find more.
    Q_PROPERTY(bool wasCancelled READ wasCancelled NOTIFY resultsChanged)
    /// Files the user has ticked for removal, and what removing them frees.
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedSizeText READ selectedSizeText NOTIFY selectionChanged)
    /// How many copies there are altogether, so a tick count has something to be
    /// a fraction of. "24 ticked" is a number; "24 of 26 copies" is a sentence
    /// somebody can check a rule against.
    Q_PROPERTY(int copyCount READ copyCount NOTIFY resultsChanged)
    /// Which rule the ticks came from, in words -- empty when nothing is ticked.
    ///
    /// A rule is applied to every group at once and then nothing says it was: the
    /// only feedback was a count and a size in the corner. This is the sentence
    /// that says what just happened, and it stops being a rule the moment
    /// somebody edits the ticks by hand, because then it is no longer true.
    Q_PROPERTY(QString ruleText READ ruleText NOTIFY selectionChanged)
    /// What a delete could not remove, one line each. A delete that failed and
    /// said nothing is indistinguishable from one that worked. See MOLE-341.
    Q_PROPERTY(QStringList deleteFailures READ deleteFailures NOTIFY resultsChanged)

public:
    DuplicatesController(PluginServices services, QObject* parent = nullptr);
    ~DuplicatesController() override;

    QStringList roots() const { return m_roots; }
    Q_INVOKABLE void setTargets(const QStringList& uris);

    QVariantList strategies() const;
    QString strategyId() const { return m_strategyId; }
    void setStrategyId(const QString& id);
    QString strategyDescription() const;
    qint64 minimumSize() const { return m_minimumSize; }
    void setMinimumSize(qint64 bytes);

    DuplicateGroupModel* groups() const { return m_groups; }
    int groupCount() const { return m_groups->rowCount(); }
    QString summary() const;
    bool isScanning() const { return !m_task.isNull(); }
    bool hasRun() const { return m_hasRun; }
    QString progressText() const { return m_progressText; }
    bool wasCancelled() const { return m_wasCancelled; }

    int selectedCount() const { return m_groups->selectedCount(); }
    QString selectedSizeText() const;
    int copyCount() const;
    QString ruleText() const { return m_ruleText; }
    /// What the last delete could not remove. Empty when everything went, and
    /// what the tab shows beside the rows that are still there.
    QStringList deleteFailures() const { return m_deleteFailures; }
    Q_INVOKABLE QStringList selectedUris() const;
    /// The ticked copies, listed for the confirmation: full location rather than
    /// name, because in a duplicate group every name is the same and the location
    /// is the only thing that tells one from another.
    Q_INVOKABLE QVariantList selectedDetails() const;
    /// The same name every other operation uses, so a chosen pile of duplicates
    /// can be added to a set, renamed or reported on without this tab knowing
    /// how any of that works.
    Q_INVOKABLE QStringList targetUris() const { return selectedUris(); }

    Q_INVOKABLE void scan();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void toggle(const QString& uri);
    Q_INVOKABLE void clearSelection();
    /// Ticks everything in each group except the one the rule keeps. The tab
    /// never picks for the user without being asked -- these are the asking.
    Q_INVOKABLE void keepNewest();
    Q_INVOKABLE void keepOldest();
    Q_INVOKABLE void keepShortestPath();
    /// Keeps this one copy and ticks every other copy in its group.
    ///
    /// The per-group half of choosing. A rule that is right for forty-nine groups
    /// and wrong for one should not have to be abandoned for the whole scan, and
    /// the alternative -- a set of rule buttons on every group -- is four controls
    /// times fifty groups to express something one click on the row already says.
    Q_INVOKABLE void keepOnly(const QString& uri);
    /// Deletes exactly these uris, or everything ticked when none are given.
    ///
    /// The argument exists because the confirmation has to delete the rows it
    /// named: a scan may still be confirming groups behind the dialog. See
    /// MOLE-339.
    Q_INVOKABLE void deleteSelected(const QStringList& uris = {});

    /// Builds a file set from the ticked copies and returns its id, or an empty
    /// string when nothing is ticked.
    ///
    /// The other way out of this tab, and the reason it matters is that a set is
    /// something every operation in Mole already takes: a result that can become
    /// one inherits copy, move, compress, rename and analyse without this view
    /// growing a single verb. Finding duplicates is *locating*, and what to do
    /// with what was found is a separate question whose answer is not always
    /// "delete".
    ///
    /// A snapshot of what is ticked, not a query that re-runs later -- the same
    /// promise LiveSearchController::buildSetFromResults() makes, and for the same
    /// reason: it is what anybody reading "make a set from this" expects.
    Q_INVOKABLE QString buildSetFromTicked(const QString& name = {});

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void rootsChanged();
    void optionsChanged();
    void resultsChanged();
    void selectionChanged();
    void progressChanged();

private:
    void selectAllBut(const QString& rule, const std::function<int(const QList<FileEntry>&)>& chooseKeeper);
    void setRuleText(const QString& text);

    PluginServices m_services;
    QStringList m_roots;
    QString m_strategyId = QStringLiteral("content");
    qint64 m_minimumSize = 1024;
    DuplicateGroupModel* m_groups = nullptr;
    bool m_hasRun = false;
    bool m_wasCancelled = false;
    QString m_ruleText;
    /// What the last scan could not read and what it left out, so the summary can
    /// qualify its own answer rather than presenting a partial one as whole.
    int m_unreadable = 0;
    int m_links = 0;
    /// What the last delete could not remove, named. Cleared by the next scan.
    QStringList m_deleteFailures;
    QString m_progressText;
    QPointer<Task> m_task;
};

class DuplicatesFeature final : public IFeature
{
public:
    explicit DuplicatesFeature(PluginServices services);

    QString id() const override { return QStringLiteral("core.duplicates"); }
    QString title() const override { return QStringLiteral("Duplicates"); }
    QString description() const override
    {
        return QStringLiteral("Find files that are the same, several ways");
    }
    QString iconText() const override { return QStringLiteral("⧉"); }
    int sortOrder() const override { return 46; }
    /// Needs roots to compare, and says so on screen when it has none: "Open
    /// this from a folder to search it." A tab that tells the user they arrived
    /// the wrong way should not have been offered.
    bool needsContext() const override { return true; }
    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

} // namespace mole
