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

Repository::Repository(std::shared_ptr<void> library, git_repository* handle, QString root)
    : m_library(std::move(library))
    , m_repo(handle)
    , m_root(std::move(root))
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

    return std::shared_ptr<Repository>(new Repository(std::move(library), handle, root));
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
