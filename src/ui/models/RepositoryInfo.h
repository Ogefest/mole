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

    /// Records what a read answered. `root` empty means there is no repository,
    /// which is the same as clear() -- so a caller can hand over whatever came
    /// back without deciding first.
    void setHead(const QString& root, const RepositoryHead& head);
    void clear();

signals:
    /// One signal for the lot. Everything here changes together, because it all
    /// comes from one read of one repository.
    void changed();

private:
    bool m_present = false;
    QString m_root;
    RepositoryHead m_head;
};

} // namespace mole
