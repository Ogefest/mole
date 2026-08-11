#include "support/GitFixture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef MOLE_HAVE_GIT2
#include <git2.h>
#endif

namespace mole::test {

bool GitFixture::isSupported()
{
#ifdef MOLE_HAVE_GIT2
    return true;
#else
    return false;
#endif
}

GitFixture::GitFixture(QString path)
    : m_path(std::move(path))
{
#ifdef MOLE_HAVE_GIT2
    git_libgit2_init();
    m_initialised = true;

    // The suite must not read whoever is running it. An empty search path for
    // each level is how libgit2 is told there is no configuration file to find;
    // otherwise a global `core.excludesFile` would quietly ignore a fixture's
    // files and the ignore tests would pass for the wrong reason.
    for (const git_config_level_t level :
        { GIT_CONFIG_LEVEL_SYSTEM, GIT_CONFIG_LEVEL_XDG, GIT_CONFIG_LEVEL_GLOBAL }) {
        git_libgit2_opts(GIT_OPT_SET_SEARCH_PATH, static_cast<int>(level), "");
    }
#endif
}

GitFixture::~GitFixture()
{
#ifdef MOLE_HAVE_GIT2
    if (m_repo)
        git_repository_free(m_repo);
    if (m_initialised)
        git_libgit2_shutdown();
#endif
}

QString GitFixture::absolute(const QString& relativePath) const
{
    return QDir(m_path).absoluteFilePath(relativePath);
}

bool GitFixture::makeDirs(const QString& relativePath)
{
    return QDir(m_path).mkpath(relativePath);
}

bool GitFixture::writeFile(const QString& relativePath, const QByteArray& contents)
{
    const QString target = absolute(relativePath);
    const QString folder = QFileInfo(target).absolutePath();
    if (!QDir().mkpath(folder))
        return false;

    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(contents) == contents.size();
}

bool GitFixture::removeFile(const QString& relativePath)
{
    return QFile::remove(absolute(relativePath));
}

#ifdef MOLE_HAVE_GIT2

namespace {

    /// Who the fixture's commits belong to. A fixed identity and a fixed time, so
    /// nothing in a test depends on the clock or on the machine's git configuration.
    git_signature* fixtureSignature()
    {
        git_signature* who = nullptr;
        if (git_signature_new(&who, "Mole Tests", "tests@mole.invalid", 1700000000, 0) != 0)
            return nullptr;
        return who;
    }

    QString shortIdOf(const git_oid& id)
    {
        char buffer[8] {};
        git_oid_tostr(buffer, sizeof(buffer), &id);
        return QString::fromLatin1(buffer);
    }

} // namespace

#endif // MOLE_HAVE_GIT2

bool GitFixture::init(const QString& initialBranch)
{
#ifdef MOLE_HAVE_GIT2
    if (m_repo)
        return false;

    git_repository_init_options options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
    const QByteArray branch = initialBranch.toUtf8();
    options.initial_head = branch.constData();
    options.flags = GIT_REPOSITORY_INIT_MKPATH;

    return git_repository_init_ext(&m_repo, m_path.toUtf8().constData(), &options) == 0;
#else
    Q_UNUSED(initialBranch);
    return false;
#endif
}

bool GitFixture::stageAll()
{
#ifdef MOLE_HAVE_GIT2
    if (!m_repo)
        return false;

    git_index* index = nullptr;
    if (git_repository_index(&index, m_repo) != 0)
        return false;

    // Two passes, because they answer different halves: add_all picks up new and
    // edited files and honours .gitignore, update_all is the one that records a
    // file the work tree no longer has.
    bool ok = git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr) == 0;
    ok = ok && git_index_update_all(index, nullptr, nullptr, nullptr) == 0;
    ok = ok && git_index_write(index) == 0;
    git_index_free(index);
    return ok;
#else
    return false;
#endif
}

QString GitFixture::commitAll(const QString& message)
{
#ifdef MOLE_HAVE_GIT2
    if (!m_repo || !stageAll())
        return {};

    git_index* index = nullptr;
    if (git_repository_index(&index, m_repo) != 0)
        return {};
    git_oid treeId {};
    const bool wroteTree = git_index_write_tree(&treeId, index) == 0;
    git_index_free(index);
    if (!wroteTree)
        return {};

    git_tree* tree = nullptr;
    if (git_tree_lookup(&tree, m_repo, &treeId) != 0)
        return {};

    git_signature* who = fixtureSignature();
    if (!who) {
        git_tree_free(tree);
        return {};
    }

    // The first commit has no parent, and asking for HEAD's is how that is told
    // apart from any other: an unborn branch simply has nothing to look up.
    git_commit* parent = nullptr;
    git_oid parentId {};
    if (git_reference_name_to_id(&parentId, m_repo, "HEAD") == 0)
        git_commit_lookup(&parent, m_repo, &parentId);

    const git_commit* parents[1] = { parent };
    git_oid commitId {};
    const int rc = git_commit_create(&commitId, m_repo, "HEAD", who, who, "UTF-8",
        message.toUtf8().constData(), tree, parent ? 1 : 0, parents);

    if (parent)
        git_commit_free(parent);
    git_signature_free(who);
    git_tree_free(tree);

    return rc == 0 ? shortIdOf(commitId) : QString();
#else
    Q_UNUSED(message);
    return {};
#endif
}

QString GitFixture::headShortId() const
{
#ifdef MOLE_HAVE_GIT2
    if (!m_repo)
        return {};
    git_oid id {};
    if (git_reference_name_to_id(&id, m_repo, "HEAD") != 0)
        return {};
    return shortIdOf(id);
#else
    return {};
#endif
}

bool GitFixture::createBranch(const QString& name)
{
#ifdef MOLE_HAVE_GIT2
    if (!m_repo)
        return false;

    git_oid headId {};
    if (git_reference_name_to_id(&headId, m_repo, "HEAD") != 0)
        return false;
    git_commit* target = nullptr;
    if (git_commit_lookup(&target, m_repo, &headId) != 0)
        return false;

    git_reference* branch = nullptr;
    const int rc = git_branch_create(&branch, m_repo, name.toUtf8().constData(), target, 0);
    if (branch)
        git_reference_free(branch);
    git_commit_free(target);
    return rc == 0;
#else
    Q_UNUSED(name);
    return false;
#endif
}

bool GitFixture::checkoutBranch(const QString& name)
{
#ifdef MOLE_HAVE_GIT2
    if (!m_repo)
        return false;

    const QByteArray reference = QByteArray("refs/heads/") + name.toUtf8();
    git_object* commit = nullptr;
    if (git_revparse_single(&commit, m_repo, reference.constData()) != 0)
        return false;

    git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
    // Force, because a fixture is built by a script that knows what it wants in
    // the work tree; a safe checkout would refuse over a file the test itself
    // has just written.
    options.checkout_strategy = GIT_CHECKOUT_FORCE;
    bool ok = git_checkout_tree(m_repo, commit, &options) == 0;
    ok = ok && git_repository_set_head(m_repo, reference.constData()) == 0;
    git_object_free(commit);
    return ok;
#else
    Q_UNUSED(name);
    return false;
#endif
}

bool GitFixture::detachHead()
{
#ifdef MOLE_HAVE_GIT2
    if (!m_repo)
        return false;
    git_oid headId {};
    if (git_reference_name_to_id(&headId, m_repo, "HEAD") != 0)
        return false;
    return git_repository_set_head_detached(m_repo, &headId) == 0;
#else
    return false;
#endif
}

bool GitFixture::beginRebase(const QString& branch, const QString& upstream)
{
#ifdef MOLE_HAVE_GIT2
    if (!m_repo)
        return false;

    const auto annotate = [this](const QString& name, git_annotated_commit** out) {
        git_reference* reference = nullptr;
        if (git_branch_lookup(&reference, m_repo, name.toUtf8().constData(), GIT_BRANCH_LOCAL) != 0)
            return false;
        const bool ok = git_annotated_commit_from_ref(out, m_repo, reference) == 0;
        git_reference_free(reference);
        return ok;
    };

    git_annotated_commit* moving = nullptr;
    git_annotated_commit* onto = nullptr;
    bool ok = annotate(branch, &moving) && annotate(upstream, &onto);

    if (ok) {
        git_rebase* rebase = nullptr;
        git_rebase_options options = GIT_REBASE_OPTIONS_INIT;
        ok = git_rebase_init(&rebase, m_repo, moving, onto, nullptr, &options) == 0;
        if (ok) {
            // One operation applied and then abandoned. Freeing the rebase
            // without finishing or aborting it leaves exactly what a rebase that
            // stopped on a conflict leaves behind.
            git_rebase_operation* operation = nullptr;
            git_rebase_next(&operation, rebase);
            git_rebase_free(rebase);
        }
    }

    if (moving)
        git_annotated_commit_free(moving);
    if (onto)
        git_annotated_commit_free(onto);
    return ok;
#else
    Q_UNUSED(branch);
    Q_UNUSED(upstream);
    return false;
#endif
}

} // namespace mole::test
