#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "ui/models/BrowserPaneController.h"

#include "core/vfs/VfsTypes.h"

namespace mole {

/// State of one browser tab: one or two panes and which of them has focus.
///
/// Dual pane is not a mode bolted on the side -- it is simply a second pane of
/// the same kind, which is why copy, move and compare between them are ordinary
/// operations rather than special cases.
class BrowserController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(mole::BrowserPaneController* left READ left CONSTANT)
    Q_PROPERTY(mole::BrowserPaneController* right READ right CONSTANT)
    Q_PROPERTY(mole::BrowserPaneController* activePane READ activePane NOTIFY activePaneChanged)
    Q_PROPERTY(mole::BrowserPaneController* otherPane READ otherPane NOTIFY activePaneChanged)
    Q_PROPERTY(ViewMode viewMode READ viewMode WRITE setViewMode NOTIFY viewModeChanged)
    Q_PROPERTY(bool splitEnabled READ splitEnabled NOTIFY viewModeChanged)
    Q_PROPERTY(bool gridEnabled READ gridEnabled NOTIFY viewModeChanged)
    Q_PROPERTY(int activePaneIndex READ activePaneIndex WRITE setActivePaneIndex NOTIFY activePaneChanged)
    /// A property, not a method. As an invokable it had no change signal, so
    /// QML evaluated the binding once and Copy/Move never enabled when the user
    /// switched to dual pane -- the condition became true with nothing to
    /// notice it.
    Q_PROPERTY(bool canTransfer READ canTransfer NOTIFY transferAvailabilityChanged)

    /// What is already known about the folder the active pane is showing, so
    /// the strip can say so instead of the user having to go and find out.
    Q_PROPERTY(bool hasReport READ hasReport NOTIFY folderFactsChanged)
    Q_PROPERTY(QString reportAgeText READ reportAgeText NOTIFY folderFactsChanged)
    Q_PROPERTY(int alertCount READ alertCount NOTIFY folderFactsChanged)
    Q_PROPERTY(int triggeredAlertCount READ triggeredAlertCount NOTIFY folderFactsChanged)
    Q_PROPERTY(bool indexed READ isIndexed NOTIFY folderFactsChanged)
    Q_PROPERTY(QString indexedText READ indexedText NOTIFY folderFactsChanged)
    /// What the current user may do here. Empty when the drive cannot say --
    /// which is a real answer for a bucket or an archive, not a gap to fill in.
    Q_PROPERTY(QString accessText READ accessText NOTIFY folderFactsChanged)
    Q_PROPERTY(QString accessDetail READ accessDetail NOTIFY folderFactsChanged)
    Q_PROPERTY(bool accessKnown READ isAccessKnown NOTIFY folderFactsChanged)
    Q_PROPERTY(bool readOnlyHere READ isReadOnlyHere NOTIFY folderFactsChanged)

public:
    enum class ViewMode {
        Single, ///< one pane, rows
        Dual, ///< two panes side by side, commander style
        Grid ///< one pane, tiles
    };
    Q_ENUM(ViewMode)

    BrowserController(
        PluginServices services, QString startUri, ViewMode initialMode, QObject* parent = nullptr);

    BrowserPaneController* left() const { return m_left; }
    BrowserPaneController* right() const { return m_right; }
    BrowserPaneController* activePane() const;
    /// The pane operations transfer *to*. Equal to activePane() in single mode.
    BrowserPaneController* otherPane() const;

    ViewMode viewMode() const { return m_viewMode; }
    void setViewMode(ViewMode mode);
    bool splitEnabled() const { return m_viewMode == ViewMode::Dual; }
    /// Tiles instead of rows. Orthogonal to how many panes there are, which is
    /// why the pane takes it as a flag rather than switching on the mode.
    bool gridEnabled() const { return m_viewMode == ViewMode::Grid; }
    int activePaneIndex() const { return m_activePaneIndex; }
    void setActivePaneIndex(int index);

    Q_INVOKABLE void toggleSplit();
    /// Moves focus to the other pane; the Tab key in a commander.
    Q_INVOKABLE void focusOtherPane();
    Q_INVOKABLE void navigateActive(const QString& uri);
    /// Points the inactive pane at the active pane's location.
    Q_INVOKABLE void mirrorToOtherPane();

    // ---- commander operations -------------------------------------------

    /// F5: copy the active pane's targets into the other pane's directory.
    Q_INVOKABLE void copyToOtherPane();
    /// F6: the same, then remove the sources once every byte arrived.
    Q_INVOKABLE void moveToOtherPane();
    /// True when a transfer would have somewhere to go and something to send.
    bool canTransfer() const;

    bool hasReport() const { return m_hasReport; }
    QString reportAgeText() const { return m_reportAgeText; }
    int alertCount() const { return m_alertCount; }
    int triggeredAlertCount() const { return m_triggeredAlertCount; }
    bool isIndexed() const { return m_indexedFiles > 0; }
    QString indexedText() const { return m_indexedText; }
    QString accessText() const { return m_accessText; }
    QString accessDetail() const { return m_accessDetail; }
    bool isAccessKnown() const { return !m_accessText.isEmpty(); }
    bool isReadOnlyHere() const { return m_readOnlyHere; }

    /// "3 items → /backup", for the confirmation prompt.
    Q_INVOKABLE QString transferSummary() const;

    /// What a transfer would actually do, for the confirmation to show before
    /// it happens: how much, where to, which names already exist there, and
    /// whether a single item could be given a different name on arrival.
    ///
    /// Collisions come from the other pane's loaded listing, so this costs
    /// nothing and can be shown as the dialog opens.
    Q_INVOKABLE QVariantMap transferPlan() const;

    /// Runs the transfer. `targetName` renames a single item on arrival; empty
    /// keeps the original. `conflict` is "skip", "overwrite" or "stop".
    Q_INVOKABLE void runTransfer(bool move, const QString& targetName, const QString& conflict);

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void transferAvailabilityChanged();
    void folderFactsChanged();
    void viewModeChanged();
    void activePaneChanged();
    void fileActivated(const QString& uri);
    void operationFailed(const QString& message);

private:
    void refreshLabels();
    void refreshFolderFacts();
    void applyAccess(const AccessInfo& access);
    void startTransfer(bool move, const QString& targetName, const QString& conflict);

    PluginServices m_services;
    BrowserPaneController* m_left = nullptr;
    BrowserPaneController* m_right = nullptr;
    ViewMode m_viewMode = ViewMode::Single;
    int m_activePaneIndex = 0;

    /// Refreshed when the active pane moves. Cached because the strip reads
    /// them on every repaint and two of the three answers come from a file.
    bool m_hasReport = false;
    QString m_reportAgeText;
    int m_alertCount = 0;
    int m_triggeredAlertCount = 0;
    qint64 m_indexedFiles = 0;
    QString m_indexedText;
    QString m_accessText;
    QString m_accessDetail;
    bool m_readOnlyHere = false;
};

/// The browsing workflow. Registered twice -- once as a plain single-pane
/// browser and once as a dual-pane commander -- so the two are separate
/// contexts in the new-tab menu while sharing every line of behaviour.
class BrowserFeature final : public IFeature
{
public:
    struct Config
    {
        QString id;
        QString title;
        QString description;
        QString iconText;
        int sortOrder = 100;
        BrowserController::ViewMode initialMode = BrowserController::ViewMode::Single;
    };

    BrowserFeature(PluginServices services, QString defaultUri, Config config);

    static Config singlePaneConfig();
    static Config dualPaneConfig();

    QString id() const override { return m_config.id; }
    QString title() const override { return m_config.title; }
    QString description() const override { return m_config.description; }
    QString iconText() const override { return m_config.iconText; }
    int sortOrder() const override { return m_config.sortOrder; }

    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
    QString m_defaultUri;
    Config m_config;
};

} // namespace mole
