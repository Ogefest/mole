#include "core/vcs/Repository.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

#ifdef MOLE_HAVE_GIT2
#include <git2.h>
#endif

namespace mole {

QString RepositoryHead::stateText() const
{
    switch (state) {
    case RepositoryState::None:
        return {};
    case RepositoryState::Merge:
        return QStringLiteral("merging");
    case RepositoryState::Revert:
        return QStringLiteral("reverting");
    case RepositoryState::CherryPick:
        return QStringLiteral("cherry-picking");
    case RepositoryState::Bisect:
        return QStringLiteral("bisecting");
    case RepositoryState::Rebase:
        return QStringLiteral("rebasing");
    case RepositoryState::ApplyMailbox:
        return QStringLiteral("applying patches");
    }
    return {};
}

QString repositoryStateMark(int state)
{
    if ((state & RepositoryConflicted) != 0)
        return QStringLiteral("U");
    if ((state & RepositoryDeleted) != 0)
        return QStringLiteral("D");
    if ((state & RepositoryRenamed) != 0)
        return QStringLiteral("R");
    if ((state & RepositoryAdded) != 0)
        return QStringLiteral("A");
    if ((state & RepositoryUntracked) != 0)
        return QStringLiteral("??");
    if ((state & RepositoryModified) != 0)
        return QStringLiteral("M");
    if ((state & RepositoryContainsChanges) != 0)
        return QStringLiteral("\u2022");
    return {};
}

bool Repository::isSupported()
{
#ifdef MOLE_HAVE_GIT2
    return true;
#else
    return false;
#endif
}

#ifdef MOLE_HAVE_GIT2

namespace {

    /// libgit2, initialised once and shut down once.
    struct Library
    {
        Library() { git_libgit2_init(); }
        ~Library() { git_libgit2_shutdown(); }

        Library(const Library&) = delete;
        Library& operator=(const Library&) = delete;
    };

    /// A hold on the library, kept by every open handle.
    ///
    /// git_libgit2_shutdown() has to come after the last git_repository_free(), and
    /// what holds the last handle is not decided here: a cache being cleared, a
    /// status walk still finishing on a worker, a pane being torn down. So the
    /// library is reference-counted by the handles themselves rather than initialised
    /// at start-up and shut down at exit, where the order between it and any static
    /// holding a handle is whatever the linker happened to choose.
    std::shared_ptr<Library> libraryHold()
    {
        static QMutex mutex;
        static std::weak_ptr<Library> weak;

        QMutexLocker locker(&mutex);
        if (std::shared_ptr<Library> held = weak.lock())
            return held;
        std::shared_ptr<Library> held = std::make_shared<Library>();
        weak = held;
        return held;
    }

    RepositoryState mapState(int state)
    {
        switch (state) {
        case GIT_REPOSITORY_STATE_MERGE:
            return RepositoryState::Merge;
        case GIT_REPOSITORY_STATE_REVERT:
        case GIT_REPOSITORY_STATE_REVERT_SEQUENCE:
            return RepositoryState::Revert;
        case GIT_REPOSITORY_STATE_CHERRYPICK:
        case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE:
            return RepositoryState::CherryPick;
        case GIT_REPOSITORY_STATE_BISECT:
            return RepositoryState::Bisect;
        case GIT_REPOSITORY_STATE_REBASE:
        case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE:
        case GIT_REPOSITORY_STATE_REBASE_MERGE:
            return RepositoryState::Rebase;
        case GIT_REPOSITORY_STATE_APPLY_MAILBOX:
        // git itself cannot tell these two apart -- the state directory a stopped
        // `git am` leaves is the one an interrupted rebase leaves. Named for the
        // patch series, which is the reading that does not claim a branch is moving.
        case GIT_REPOSITORY_STATE_APPLY_MAILBOX_OR_REBASE:
            return RepositoryState::ApplyMailbox;
        default:
            return RepositoryState::None;
        }
    }

    /// Seven hex characters, the length git itself abbreviates to by default.
    QString shortIdOf(const git_oid* oid)
    {
        if (!oid)
            return {};
        char buffer[8] {};
        git_oid_tostr(buffer, sizeof(buffer), oid);
        return QString::fromLatin1(buffer);
    }

    /// git's own flags, folded into the six states a listing can draw.
    ///
    /// Index and work tree are folded together on purpose: Mole shows what is
    /// different from the last commit, and "staged" is a distinction only a git
    /// client that can unstage has any use for -- this one cannot (ADR-0041).
    int mapStatusFlags(unsigned int flags)
    {
        int out = RepositoryUnchanged;
        // Conflicted first, and on its own: a conflicted path also carries
        // modified bits, and "modified" is the less urgent half of that truth.
        if ((flags & GIT_STATUS_CONFLICTED) != 0)
            return RepositoryConflicted;
        if ((flags & (GIT_STATUS_INDEX_NEW)) != 0)
            out |= RepositoryAdded;
        if ((flags & GIT_STATUS_WT_NEW) != 0)
            out |= RepositoryUntracked;
        if ((flags
                & (GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_WT_MODIFIED | GIT_STATUS_INDEX_TYPECHANGE
                    | GIT_STATUS_WT_TYPECHANGE))
            != 0)
            out |= RepositoryModified;
        if ((flags & (GIT_STATUS_INDEX_DELETED | GIT_STATUS_WT_DELETED)) != 0)
            out |= RepositoryDeleted;
        if ((flags & (GIT_STATUS_INDEX_RENAMED | GIT_STATUS_WT_RENAMED)) != 0)
            out |= RepositoryRenamed;
        return out;
    }

    /// Records one changed path, and marks every directory above it.
    ///
    /// `relative` is as git spells it, from the work tree root.
    void recordStatus(RepositoryStatus& out, const QString& root, QString relative, int state)
    {
        // An untracked directory arrives with a trailing separator, because that is
        // what git reports when nothing inside it is tracked -- `?? newdir/`. The row
        // that stands for it in a listing is the directory, so the separator comes
        // off and the mark lands on the folder rather than on a path nothing shows.
        while (relative.endsWith(QLatin1Char('/')))
            relative.chop(1);
        if (relative.isEmpty())
            return;

        out.byPath[root + QLatin1Char('/') + relative] |= state;

        // And every directory above it, up to but not including the work tree root:
        // a folder on screen is marked when anything below it has changed, however
        // deep. The root itself is left out because a pane showing it is a pane
        // inside the checkout, not a row standing for it.
        for (int slash = relative.lastIndexOf(QLatin1Char('/')); slash > 0;
             slash = relative.lastIndexOf(QLatin1Char('/'))) {
            relative.truncate(slash);
            out.byPath[root + QLatin1Char('/') + relative] |= RepositoryContainsChanges;
        }
    }

    /// The path a listing can actually mark, out of a delta that may name two.
    ///
    /// For a rename that is the destination: the source is not on disk any more, so
    /// marking it puts the letter on a row nothing draws, and the file somebody can
    /// see goes unmarked. For everything else the two are the same path.
    QString pathToMark(const git_diff_delta* delta)
    {
        if (!delta)
            return {};
        if (delta->new_file.path)
            return QString::fromUtf8(delta->new_file.path);
        if (delta->old_file.path)
            return QString::fromUtf8(delta->old_file.path);
        return {};
    }

    /// The work tree path libgit2 answers with, in the form the rest of Mole uses:
    /// no trailing separator, so it can be compared with a uri's path and used as a
    /// map key without two spellings of one directory.
    QString normalisedRoot(const char* workdir)
    {
        if (!workdir)
            return {};
        QString path = QString::fromUtf8(workdir);
        while (path.size() > 1 && (path.endsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('\\'))))
            path.chop(1);
        return QDir::cleanPath(path);
    }

    /// Where the repository's own directory is -- `<root>/.git` for an ordinary
    /// checkout, somewhere else entirely for a linked work tree or a submodule.
    ///
    /// This is the cheap half of opening: it finds the repository without reading a
    /// single reference, which is what makes it worth doing before the cache is
    /// consulted.
    QString discoverGitDir(const QString& path)
    {
        const QString start = QFileInfo(path).absoluteFilePath();
        if (start.isEmpty())
            return {};

        const std::shared_ptr<Library> library = libraryHold();
        git_buf found = GIT_BUF_INIT;
        // No ceiling directories: the search stops at the filesystem root, which is
        // what git does and what somebody with a checkout at the top of a mount
        // expects. Across filesystems too -- a checkout on a mounted disk inside a
        // home directory is one work tree, not two.
        const int rc = git_repository_discover(&found, start.toUtf8().constData(), 1, nullptr);
        QString answer;
        if (rc == 0)
            answer = normalisedRoot(found.ptr);
        git_buf_dispose(&found);
        return answer;
    }

    /// The branch HEAD names before anything has been committed to it.
    ///
    /// `git init` leaves HEAD pointing at a branch that does not exist yet, so the
    /// reference cannot be resolved and its shorthand cannot be asked for. The name
    /// is still the right thing to show: somebody who has just run `git init` is on
    /// `main`, whatever git's own answer to "which commit" is.
    QString unbornBranchName(git_repository* repo)
    {
        git_reference* head = nullptr;
        if (git_reference_lookup(&head, repo, "HEAD") != 0 || !head)
            return {};

        QString name;
        if (const char* target = git_reference_symbolic_target(head))
            name = QString::fromUtf8(target);
        git_reference_free(head);

        static const QString prefix = QStringLiteral("refs/heads/");
        return name.startsWith(prefix) ? name.mid(prefix.size()) : name;
    }

} // namespace

#endif // MOLE_HAVE_GIT2

Repository::Repository(std::shared_ptr<void> library, git_repository* handle, QString root, QString gitDir)
    : m_library(std::move(library))
    , m_repo(handle)
    , m_root(std::move(root))
    , m_gitDir(std::move(gitDir))
{
}

Repository::~Repository()
{
#ifdef MOLE_HAVE_GIT2
    if (m_repo)
        git_repository_free(m_repo);
#endif
}

QString Repository::workTreeFor(const QString& path)
{
    // Through the cache rather than by opening: the question is asked on every
    // navigation, and the answer for a folder inside a checkout already visited
    // costs one discovery.
    if (const std::shared_ptr<Repository> repo = RepositoryCache::shared().forPath(path))
        return repo->root();
    return {};
}

std::shared_ptr<Repository> Repository::open(const QString& path)
{
#ifdef MOLE_HAVE_GIT2
    const QString start = QFileInfo(path).absoluteFilePath();
    if (start.isEmpty())
        return nullptr;

    std::shared_ptr<Library> library = libraryHold();
    git_repository* handle = nullptr;
    // Searches upwards from `start`: this is what makes a folder several levels
    // deep inside a checkout answer with the checkout rather than with nothing.
    if (git_repository_open_ext(&handle, start.toUtf8().constData(), 0, nullptr) != 0) {
        return nullptr;
    }
    if (!handle)
        return nullptr;

    const QString root = normalisedRoot(git_repository_workdir(handle));
    if (root.isEmpty()) {
        // A bare repository has no work tree. Nothing Mole shows is inside one,
        // so there is nothing to mark and no branch worth claiming for a folder
        // that only happens to sit next to it.
        git_repository_free(handle);
        return nullptr;
    }

    const QString gitDir = normalisedRoot(git_repository_path(handle));
    return std::shared_ptr<Repository>(new Repository(std::move(library), handle, root, gitDir));
#else
    Q_UNUSED(path);
    return nullptr;
#endif
}

RepositoryHead Repository::head() const
{
    RepositoryHead out;
#ifdef MOLE_HAVE_GIT2
    QMutexLocker locker(&m_mutex);
    if (!m_repo)
        return out;

    out.state = mapState(git_repository_state(m_repo));

    git_reference* ref = nullptr;
    const int rc = git_repository_head(&ref, m_repo);
    if (rc == GIT_EUNBORNBRANCH) {
        out.unborn = true;
        out.branch = unbornBranchName(m_repo);
        return out;
    }
    if (rc != 0 || !ref) {
        if (ref)
            git_reference_free(ref);
        return out;
    }

    // Asked of the repository rather than read off the reference: HEAD resolved
    // to a commit is a reference whose shorthand is the literal "HEAD", and
    // showing that as a branch name is exactly the wrong answer.
    if (git_repository_head_detached(m_repo) > 0)
        out.detached = true;
    else if (const char* name = git_reference_shorthand(ref))
        out.branch = QString::fromUtf8(name);

    out.shortId = shortIdOf(git_reference_target(ref));
    git_reference_free(ref);
#endif
    return out;
}

RepositoryStatus Repository::readStatus(const CancelToken& cancel) const
{
    RepositoryStatus out;
#ifdef MOLE_HAVE_GIT2
    QMutexLocker locker(&m_mutex);
    if (!m_repo)
        return out;
    if (cancel.isCancelled())
        return out;

    git_status_options options = GIT_STATUS_OPTIONS_INIT;
    options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    // Untracked files are included because a file the user has just made is the
    // most interesting thing in a folder, and .gitignore is honoured because
    // libgit2 honours it unless told otherwise. GIT_STATUS_OPT_INCLUDE_IGNORED is
    // deliberately absent: a build directory would otherwise bury every real
    // change under thousands of marks nobody wants.
    //
    // Untracked directories are not recursed into either. git reports a folder
    // whose contents are all untracked as the folder, and that is the row a
    // listing has -- recursing would report a hundred files inside a new
    // directory and count a hundred changes where git counts one.
    options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX
        | GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;

    // The list rather than git_status_foreach_ext, for the renames. The callback
    // form is handed one path per entry, and for a rename that path is the source --
    // which is not on disk any more, so the letter would land on a row nothing draws
    // while the file somebody can see went unmarked. An entry carries both deltas
    // and so both paths.
    //
    // It costs nothing in reach: foreach_ext is itself a list built in full and then
    // iterated, so a token polled in its callback never interrupted the stat pass
    // either. libgit2 offers no hook inside that pass, so cancellation is checked
    // per entry here, and what an abandoned walk saves is the answer being carried
    // any further -- not the walk. See TODO.md.
    git_status_list* list = nullptr;
    if (git_status_list_new(&list, m_repo, &options) != 0)
        return out;

    const size_t entries = git_status_list_entrycount(list);
    for (size_t i = 0; i < entries; ++i) {
        if (cancel.isCancelled()) {
            git_status_list_free(list);
            // Empty rather than partial. Half a walk marks half a listing correctly
            // and the rest as clean, and a listing that calls a changed file
            // unchanged is worse than one that says nothing at all.
            return RepositoryStatus {};
        }

        const git_status_entry* entry = git_status_byindex(list, i);
        if (!entry)
            continue;
        const int state = mapStatusFlags(entry->status);
        if (state == RepositoryUnchanged)
            continue;

        // One entry is one change however many paths name it, so that the count
        // agrees with what `git status` says.
        ++out.changedCount;
        const QString staged = pathToMark(entry->head_to_index);
        const QString working = pathToMark(entry->index_to_workdir);
        if (!staged.isEmpty())
            recordStatus(out, m_root, staged, state);
        if (!working.isEmpty() && working != staged)
            recordStatus(out, m_root, working, state);
    }

    git_status_list_free(list);
    out.complete = !cancel.isCancelled();
#else
    Q_UNUSED(cancel);
#endif
    return out;
}

RepositoryStatusCache& RepositoryStatusCache::shared()
{
    static RepositoryStatusCache cache;
    return cache;
}

RepositoryStatus RepositoryStatusCache::forRoot(const QString& root) const
{
    QMutexLocker locker(&m_mutex);
    return m_byRoot.value(root);
}

void RepositoryStatusCache::store(const QString& root, RepositoryStatus status)
{
    if (root.isEmpty() || !status.complete)
        return;
    QMutexLocker locker(&m_mutex);
    m_byRoot.insert(root, std::move(status));
}

void RepositoryStatusCache::forget(const QString& root)
{
    QMutexLocker locker(&m_mutex);
    m_byRoot.remove(root);
}

void RepositoryStatusCache::clear()
{
    QMutexLocker locker(&m_mutex);
    m_byRoot.clear();
}

RepositoryCache& RepositoryCache::shared()
{
    static RepositoryCache cache;
    return cache;
}

std::shared_ptr<Repository> RepositoryCache::forPath(const QString& path)
{
#ifdef MOLE_HAVE_GIT2
    // Discovery first, and outside the lock: it touches the filesystem, and a
    // path with no repository above it must not hold up a pane asking about a
    // checkout it has already opened.
    const QString gitDir = discoverGitDir(path);
    if (gitDir.isEmpty())
        return nullptr;

    QMutexLocker locker(&m_mutex);
    const auto found = m_byGitDir.constFind(gitDir);
    if (found != m_byGitDir.constEnd())
        return *found;

    std::shared_ptr<Repository> repo = Repository::open(path);
    if (!repo)
        return nullptr;
    m_byGitDir.insert(gitDir, repo);
    return repo;
#else
    Q_UNUSED(path);
    return nullptr;
#endif
}

void RepositoryCache::clear()
{
    QMutexLocker locker(&m_mutex);
    m_byGitDir.clear();
}

} // namespace mole
