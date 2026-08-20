#pragma once

#include <QAbstractListModel>
#include <QString>

namespace mole {

class ActionRegistry;
class BookmarkModel;
class DriveListModel;

/// Everything that can be done right now, as one filterable list.
///
/// Deliberately a *view* over the registries that already exist rather than a
/// list of its own: the menu entries come from ActionRegistry, the places from
/// BookmarkModel and DriveListModel. A second list maintained by hand would drift
/// out of step with the menu the first time somebody added an action, and the
/// whole value of this is that it can be trusted to hold everything.
///
/// Only what is available appears. The menu already evaluates each entry's
/// `enabled` callback when it is asked, so a greyed-out action is simply absent
/// here -- offering something that cannot run is worse than not offering it.
class CommandPaletteModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    /// Narrows the list. Matched against the whole path, so both "termi" and
    /// "op term" find Operations -> Terminal here.
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)

public:
    /// What the palette can do to a drive, as opposed to where it can take you.
    /// A drive you can only connect by finding a small button with the pointer
    /// is half-built in an application whose palette is described as the one
    /// key that reaches everything.
    enum class DriveCommand { Connect, Eject, Check, Unlock };
    Q_ENUM(DriveCommand)

    enum Role {
        /// What to show: "Terminal here".
        TitleRole = Qt::UserRole + 1,
        /// Where it came from: "Operations", "Bookmarks", "Drives".
        GroupRole,
        /// The two together, which is what is matched against.
        PathRole,
        /// The keys that already do this, when there are any.
        ShortcutRole,
        IconTextRole,
    };

    /// Any of these may be null; the palette then simply has less in it, which is
    /// what a test that only cares about menu entries wants.
    CommandPaletteModel(
        ActionRegistry* actions, BookmarkModel* bookmarks, DriveListModel* drives, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString filter() const { return m_filter; }
    void setFilter(const QString& filter);

    /// Rebuilt from the registries. Called every time the palette opens, because
    /// what is available depends on the tab in front of the user.
    Q_INVOKABLE void refresh();

    /// Runs the row. Emits the request rather than acting, so this model needs to
    /// know nothing about tabs or navigation.
    Q_INVOKABLE void activate(int row);

signals:
    void countChanged();
    void filterChanged();
    /// A menu entry was chosen.
    void actionRequested(const QString& actionId);
    /// A drive was chosen, or anywhere else named by a uri alone.
    void locationRequested(const QString& uri);
    /// A bookmark was chosen. Carries the kind beside the target, because a set
    /// bookmark's target is an id rather than a place. See ADR-0061.
    void bookmarkRequested(const QString& kind, const QString& target);
    /// Something is to be *done* to a drive rather than gone to. Emitted rather
    /// than acted on, like everything else here: this model knows what can be
    /// done and nothing about how.
    void driveCommandRequested(mole::CommandPaletteModel::DriveCommand what, const QString& driveId);

private:
    struct Command
    {
        QString title;
        QString group;
        QString shortcut;
        QString iconText;
        /// Exactly one of the three is set: a menu entry, a place to go, or
        /// something to do to a drive.
        QString actionId;
        QString uri;
        QString driveId;
        DriveCommand verb = DriveCommand::Check;
        /// Set only on a bookmark row: "folder" or "set". The kind travels beside
        /// the target the same way `verb` travels beside a drive id, because a set
        /// bookmark's target is not somewhere goTo() can go.
        QString bookmarkKind;

        QString path() const { return group + QStringLiteral(" → ") + title; }
    };

    void rebuildVisible();
    /// How well `command` answers `m_filter`, or -1 for not at all. Higher wins.
    static int score(const Command& command, const QString& needle);

    ActionRegistry* m_actions = nullptr;
    BookmarkModel* m_bookmarks = nullptr;
    DriveListModel* m_drives = nullptr;

    QList<Command> m_all;
    QList<Command> m_visible;
    QString m_filter;
};

} // namespace mole
