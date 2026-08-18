#pragma once

#include "core/vfs/VfsTypes.h"

#include <QDateTime>
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

    /// Whether the branch has a tracking branch configured at all.
    ///
    /// Reported separately from the counts because without an upstream there is
    /// nothing to be ahead or behind *of*, and a bare `0/0` reads as up to date when
    /// the truth is that there is nothing to compare against.
    bool hasUpstream = false;
    /// How far the branch is from its remote-tracking reference **as it was last
    /// fetched**.
    ///
    /// Not a question about the remote as it is now: nothing in this class talks to a
    /// network, and a band implying that it had would be the most misleading thing on
    /// the screen. Both nought when the branch and its upstream agree.
    int ahead = 0;
    int behind = 0;

    /// The subject line of the commit HEAD points at -- the first line of its
    /// message, which is how somebody recognises where they left off.
    QString subject;
    /// When that commit was made. Invalid in a repository with no commits, which is
    /// what tells a band to draw no commit line rather than an empty one.
    QDateTime committedAt;

    /// Whether anything at all could be read. False for a path outside a work
    /// tree, and for a build without git support.
    bool isValid() const { return !branch.isEmpty() || detached || unborn; }

    /// What to call the interrupted state, in the present tense -- "rebasing".
    /// Empty when nothing is interrupted.
    QString stateText() const;
};

/// What git says about one path in a work tree.
///
/// A bitmask rather than one value, because a path really is several of these at
/// once: a file staged as added and edited again since is added *and* modified,
/// and a listing forced to choose between them would be choosing which half of
/// the truth to show.
enum RepositoryFileState {
    RepositoryUnchanged = 0,
    RepositoryModified = 1 << 0,
    RepositoryAdded = 1 << 1,
    RepositoryDeleted = 1 << 2,
    RepositoryUntracked = 1 << 3,
    RepositoryRenamed = 1 << 4,
    RepositoryConflicted = 1 << 5,
    /// Something inside this directory is one of the above. Directories only.
    ///
    /// Rolled up by the walk rather than by whoever draws a listing, because git
    /// answers with paths to files and a listing shows folders -- without this,
    /// opening a checkout at its root shows a clean-looking list of directories
    /// over a tree full of edits. It does not say *which* of the states is inside,
    /// because none of them aggregates into anything true.
    RepositoryContainsChanges = 1 << 6,
};

/// Every state git itself reports, as one mask.
///
/// What separates a path git named from a directory this walk rolled up above
/// one, which is the question anybody listing the changes has to ask: a folder
/// carrying nothing but RepositoryContainsChanges is Mole's own arithmetic, not
/// git's answer.
constexpr int RepositoryReportedStates = RepositoryModified | RepositoryAdded | RepositoryDeleted
    | RepositoryUntracked | RepositoryRenamed | RepositoryConflicted;

/// The one mark a listing puts on a row in this state.
///
/// git's own letters -- `M`, `A`, `D`, `??`, `R`, `U` -- because anybody with a
/// checkout reads them without thinking, and because the letter carries the
/// meaning on its own: colour is never the only signal (ADR-0010). A directory
/// that only inherited the roll-up gets a dot, since it stands for *something
/// below here* rather than for a state of its own.
///
/// One mark for a path that is several states at once, most urgent first: a
/// conflict, then a deletion, a rename, an addition, an untracked file, an edit.
/// Structure beats "modified", which is the thing most likely to be true anyway.
QString repositoryStateMark(int state);

/// What one walk of a work tree found.
struct RepositoryStatus
{
    /// Flags per absolute path -- files as git reported them, and the directories
    /// above them rolled up. Absolute rather than relative to the work tree,
    /// because every caller has a path in hand and none of them has a root.
    QHash<QString, int> byPath;
    /// How many paths git itself reported. Not `byPath.size()`, which also counts
    /// the directories above them.
    int changedCount = 0;
    /// Whether the walk ran to the end. A cancelled walk answers with whatever it
    /// had reached, which is not an answer anybody may show.
    bool complete = false;

    int stateFor(const QString& absolutePath) const
    {
        return byPath.value(absolutePath, RepositoryUnchanged);
    }
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

    /// The repository's own directory -- `<root>/.git` for an ordinary checkout,
    /// somewhere else entirely for a linked work tree or a submodule.
    ///
    /// Wanted by whoever has to notice a commit made outside Mole: what changes
    /// then is in here, not in the work tree.
    const QString& gitDir() const { return m_gitDir; }

    /// Which branch, or that HEAD is detached, and whether git is part-way
    /// through something. One lock and a handful of reference reads: no work tree
    /// walk, so this is cheap enough to answer on every navigation.
    RepositoryHead head() const;

    /// Walks the work tree and answers what has changed.
    ///
    /// The expensive one, and the reason ReadStatusTask exists: a stat per file,
    /// which on a checkout of any size takes long enough to be felt. Cancellation
    /// is polled once per path, so navigating away abandons the walk rather than
    /// finishing it for nobody.
    ///
    /// Holds this repository for the whole walk, which is what one shared handle
    /// costs -- a second pane asking which branch it is on waits. That is why the
    /// answer is cached in RepositoryStatusCache and not re-read per folder.
    RepositoryStatus readStatus(const CancelToken& cancel) const;

private:
    Repository(std::shared_ptr<void> library, git_repository* handle, QString root, QString gitDir);

    /// Keeps libgit2 initialised for as long as any handle is open. See the
    /// comment on libraryHold() in the implementation.
    std::shared_ptr<void> m_library;
    git_repository* m_repo = nullptr;
    QString m_root;
    QString m_gitDir;
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

/// The last completed walk for each work tree, shared by every pane showing one.
///
/// A cache of its own rather than a field on Repository, and the reason is the
/// lock. A Repository is held for the whole of a status walk, so a pane reading
/// the answer off it would wait for that walk to finish -- on the UI thread, which
/// is the one rule this application does not bend. This has a mutex of its own and
/// never holds it for longer than a hash lookup.
///
/// Keyed by work tree root, which is what makes one walk serve every folder in a
/// checkout: navigating from `src/` to `tests/` finds the answer already here and
/// starts nothing.
class RepositoryStatusCache
{
public:
    static RepositoryStatusCache& shared();

    /// What the last completed walk of `root` found. `complete` is false when
    /// nothing has walked it yet, or when the answer has been forgotten.
    RepositoryStatus forRoot(const QString& root) const;
    void store(const QString& root, RepositoryStatus status);
    /// Forgets one work tree's answer, so the next pane that asks walks again.
    /// What an operation writing inside it leaves behind.
    void forget(const QString& root);
    void clear();

private:
    RepositoryStatusCache() = default;

    mutable QMutex m_mutex;
    QHash<QString, RepositoryStatus> m_byRoot;
};

} // namespace mole

// Crosses a thread boundary: the branch is read on a pool thread and the band
// that shows it is drawn on the other one.
Q_DECLARE_METATYPE(mole::RepositoryHead)
// The same, for what a work tree walk found.
Q_DECLARE_METATYPE(mole::RepositoryStatus)
