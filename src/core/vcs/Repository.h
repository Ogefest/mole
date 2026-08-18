#pragma once

#include <QHash>
#include <QMetaType>
#include <QMutex>
#include <QString>

#include <memory>

// Forward-declared rather than including <git2.h>. This header is read from the
// interface layer, where a handle is an opaque pointer and the whole of libgit2's
// headers would be paid for by every translation unit that only wants to ask
// which branch a folder is on.
struct git_repository;

namespace mole {

/// What git is in the middle of, when it is in the middle of something.
///
/// During any of these the branch is not the interesting fact -- a rebase stopped
/// on a conflict shows a branch name that is either the old one or a detached
/// head, and either reading is wrong about what is going on.
enum class RepositoryState {
    None, ///< nothing interrupted
    Merge,
    Revert,
    CherryPick,
    Bisect,
    Rebase,
    ApplyMailbox, ///< `git am`, a patch series being applied
};

/// Where a work tree's HEAD is pointing.
struct RepositoryHead
{
    /// Branch shorthand -- "main", "feature/thing". Empty when HEAD is detached.
    QString branch;
    /// The abbreviated commit id HEAD resolves to. Empty in a repository with no
    /// commits yet.
    QString shortId;
    /// HEAD names a commit rather than a branch. Reported rather than left as an
    /// empty branch name, which reads as a fault in Mole rather than as a fact
    /// about the checkout.
    bool detached = false;
    /// A repository with no commits: the branch exists as a name in HEAD and has
    /// nothing to point at yet. `git init` followed by nothing looks like this,
    /// and so does a first clone that has not fetched.
    bool unborn = false;
    RepositoryState state = RepositoryState::None;

    /// Whether anything at all could be read. False for a path outside a work
    /// tree, and for a build without git support.
    bool isValid() const { return !branch.isEmpty() || detached || unborn; }

    /// What to call the interrupted state, in the present tense -- "rebasing".
    /// Empty when nothing is interrupted.
    QString stateText() const;
};

/// One work tree, read and never written.
///
/// The read-only boundary is deliberate and belongs to the whole feature rather
/// than to this class: Mole shows git state and does not change it. See
/// docs/adr/0041-git-state-is-read-through-libgit2.md.
///
/// THREADING
/// ---------
/// A `git_repository` must not be used from two threads at once, and handles are
/// shared -- one per work tree, handed out by RepositoryCache to every pane and
/// every task that asks. So every method here takes the object's own lock, and
/// callers may hold the same Repository from any thread. What that costs is that
/// two panes asking about one checkout serialise; what it buys is that opening a
/// repository, which is the expensive part, happens once.
class Repository
{
public:
    ~Repository();

    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    /// Whether Mole was built with git support at all. Everything below answers
    /// "nothing here" when this is false, so a caller never needs to ask first.
    static bool isSupported();

    /// The work tree root that contains `path`, or an empty string when there is
    /// none. Walks up from `path` the way git itself does, so any folder inside a
    /// checkout answers with the checkout.
    ///
    /// Answered through the cache, so a folder inside a checkout something has
    /// already asked about costs one discovery and no open.
    static QString workTreeFor(const QString& path);

    /// Opens the work tree containing `path`. Null when there is none, or when
    /// git support is compiled out.
    ///
    /// Prefer RepositoryCache: opening is the expensive part and every folder
    /// inside one work tree shares the answer.
    static std::shared_ptr<Repository> open(const QString& path);

    /// Absolute path of the work tree root, without a trailing separator.
    const QString& root() const { return m_root; }

    /// Which branch, or that HEAD is detached, and whether git is part-way
    /// through something. One lock and a handful of reference reads: no work tree
    /// walk, so this is cheap enough to answer on every navigation.
    RepositoryHead head() const;

private:
    Repository(std::shared_ptr<void> library, git_repository* handle, QString root);

    /// Keeps libgit2 initialised for as long as any handle is open. See the
    /// comment on libraryHold() in the implementation.
    std::shared_ptr<void> m_library;
    git_repository* m_repo = nullptr;
    QString m_root;
    mutable QMutex m_mutex;
};

/// One open handle per repository, shared by everything that asks about it.
///
/// Keyed by the repository's own directory rather than by the path queried,
/// because that is what makes navigating from `src/` to `tests/` inside one
/// checkout free, and because discovering it is the cheap half of opening. A
/// process scope rather than a per-pane one for the same reason: two panes on one
/// checkout are two views of one repository.
class RepositoryCache
{
public:
    static RepositoryCache& shared();

    /// The repository containing `path`, or null when there is none. Thread-safe.
    std::shared_ptr<Repository> forPath(const QString& path);

    /// Forgets every handle held.
    ///
    /// Nothing in normal use needs this: every lookup discovers the repository on
    /// disk before the map is consulted, so a work tree that has been deleted is
    /// never answered for. It exists for the case discovery cannot see -- a
    /// checkout deleted and made again at the same path, where the old handle
    /// would still be found -- and for tests, which do that on purpose.
    void clear();

private:
    RepositoryCache() = default;

    QMutex m_mutex;
    QHash<QString, std::shared_ptr<Repository>> m_byGitDir;
};

} // namespace mole

// Crosses a thread boundary: the branch is read on a pool thread and the band
// that shows it is drawn on the other one.
Q_DECLARE_METATYPE(mole::RepositoryHead)
