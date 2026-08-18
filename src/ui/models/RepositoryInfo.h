#pragma once

#include "core/vcs/Repository.h"

#include <QObject>
#include <QString>

namespace mole {

/// What the band above a listing has to say about the folder in view.
///
/// One of these per pane, filled by BrowserPaneController from a worker and never
/// written to by QML. Deliberately passive, the way FileListModel is: it holds an
/// answer somebody else read, so it can be asserted headlessly without a window
/// and without a repository.
///
/// `present` is false -- and the band absent rather than empty -- in all three
/// cases where there is nothing to say: a folder in no work tree, a folder on a
/// drive that is not a real filesystem, and a build without libgit2.
class RepositoryInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool present READ isPresent NOTIFY changed)
    Q_PROPERTY(QString root READ root NOTIFY changed)
    Q_PROPERTY(QString branch READ branch NOTIFY changed)
    Q_PROPERTY(bool detached READ isDetached NOTIFY changed)
    Q_PROPERTY(QString shortId READ shortId NOTIFY changed)
    Q_PROPERTY(QString stateText READ stateText NOTIFY changed)
    Q_PROPERTY(QString headText READ headText NOTIFY changed)
    /// Whether a work tree walk has answered. False until it has, which is why
    /// the band shows a branch before it shows a count rather than showing
    /// "clean" for the moment before the walk lands.
    Q_PROPERTY(bool statusKnown READ isStatusKnown NOTIFY changed)
    Q_PROPERTY(int changedCount READ changedCount NOTIFY changed)
    Q_PROPERTY(QString changesText READ changesText NOTIFY changed)
    /// How far the branch is from what it tracks, in words. Empty when there is no
    /// upstream and when the two agree -- both of which mean there is nothing to do.
    Q_PROPERTY(QString trackingText READ trackingText NOTIFY changed)
    /// Whether there is a commit to name at all. False in a repository with no
    /// commits, which is what leaves the band with no commit line rather than an
    /// empty one.
    Q_PROPERTY(bool hasCommit READ hasCommit NOTIFY changed)
    Q_PROPERTY(QString commitSubject READ commitSubject NOTIFY changed)
    Q_PROPERTY(QString commitAge READ commitAge NOTIFY changed)

public:
    explicit RepositoryInfo(QObject* parent = nullptr);

    bool isPresent() const { return m_present; }
    const QString& root() const { return m_root; }
    QString branch() const { return m_head.branch; }
    bool isDetached() const { return m_head.detached; }
    QString shortId() const { return m_head.shortId; }
    QString stateText() const { return m_head.stateText(); }

    /// The one line the band shows about where HEAD is.
    ///
    /// Three answers rather than one, in this order: what git is part-way through
    /// when it is part-way through something, because during a rebase the branch
    /// name is not the fact anybody needs; that HEAD is detached and at which
    /// commit, because an empty branch name reads as a fault in Mole; and
    /// otherwise the branch.
    QString headText() const;

    bool isStatusKnown() const { return m_statusKnown; }
    int changedCount() const { return m_status.changedCount; }

    /// How much of the work tree differs from the last commit.
    ///
    /// Empty until a walk has answered. A clean tree says "clean" rather than
    /// "0 changed", because a count of nought is a sentence about arithmetic and
    /// what somebody wants to know is whether there is anything to deal with.
    QString changesText() const;

    QString trackingText() const;
    bool hasCommit() const { return m_present && m_head.committedAt.isValid(); }
    QString commitSubject() const { return hasCommit() ? m_head.subject : QString {}; }
    /// How long ago the commit was made, in the shape the rest of Mole uses.
    QString commitAge() const;

    /// What the walk found, for whoever marks the rows. Empty until it has.
    const RepositoryStatus& status() const { return m_status; }

    /// Records what a read answered. `root` empty means there is no repository,
    /// which is the same as clear() -- so a caller can hand over whatever came
    /// back without deciding first.
    ///
    /// A different root discards the status: it belonged to the other checkout,
    /// and showing one repository's count beside another's branch would be a wrong
    /// answer rather than a missing one.
    void setHead(const QString& root, const RepositoryHead& head);

    /// Records what a walk of `root` found. Ignored when it is not the work tree
    /// in view, which is what an answer about a folder somebody has left looks
    /// like by the time it arrives.
    void setStatus(const QString& root, const RepositoryStatus& status);
    void clearStatus();
    void clear();

signals:
    /// One signal for the lot. Everything here changes together, because it all
    /// comes from one read of one repository.
    void changed();

private:
    bool m_present = false;
    QString m_root;
    RepositoryHead m_head;
    bool m_statusKnown = false;
    RepositoryStatus m_status;
};

} // namespace mole
