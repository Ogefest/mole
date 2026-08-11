#pragma once

#include <QByteArray>
#include <QString>

struct git_repository;

namespace mole::test {

/// A real git work tree in a temporary directory, built through libgit2 itself.
///
/// No `git` process and no network: everything here is library calls, so the
/// suites that need a checkout run on a machine with no git installed and stay
/// as cheap as any other test. And no user configuration either -- libgit2's
/// search paths for the system, global and XDG config files are emptied, and
/// commits carry a signature this class supplies. A developer whose
/// `~/.gitconfig` sets `init.defaultBranch` or `core.excludesFile` gets the same
/// answers as the machine that has neither.
///
/// Every method answers false or an empty string on failure rather than
/// asserting, so the test decides what a failure to build a fixture means.
class GitFixture
{
public:
    /// Whether Mole was built with git support. Everything else here needs it,
    /// and a suite without it should skip rather than fail.
    static bool isSupported();

    /// Wraps `path`, which must already exist. Nothing happens on disk until
    /// init().
    explicit GitFixture(QString path);
    ~GitFixture();

    GitFixture(const GitFixture&) = delete;
    GitFixture& operator=(const GitFixture&) = delete;

    /// Creates the repository. The initial branch is named explicitly rather
    /// than left to git's default, which differs between versions and can be
    /// overridden by whoever is running the suite.
    bool init(const QString& initialBranch = QStringLiteral("main"));

    bool isValid() const { return m_repo != nullptr; }
    const QString& path() const { return m_path; }
    QString absolute(const QString& relativePath) const;

    // ---- the work tree ---------------------------------------------------

    bool writeFile(const QString& relativePath, const QByteArray& contents = "x");
    bool removeFile(const QString& relativePath);
    bool makeDirs(const QString& relativePath);

    // ---- history ---------------------------------------------------------

    /// Stages everything in the work tree -- additions, edits and deletions --
    /// and commits it. Returns the new commit's abbreviated id.
    QString commitAll(const QString& message);
    /// Stages without committing, which is what an added-but-not-committed file
    /// has to be in for status to call it added.
    bool stageAll();

    /// The abbreviated id HEAD points at, empty in a repository with no commits.
    QString headShortId() const;

    // ---- branches and states ---------------------------------------------

    bool createBranch(const QString& name);
    bool checkoutBranch(const QString& name);
    /// Points HEAD at the commit it is already on, rather than at the branch.
    bool detachHead();

    /// Starts a rebase of `branch` onto `upstream` and walks away from it.
    ///
    /// Which is what an interrupted rebase is: the state directory is on disk and
    /// nothing has finished or aborted it. Built through the rebase machinery
    /// rather than by writing that directory by hand, so the fixture is a
    /// situation git really produces rather than our idea of one.
    bool beginRebase(const QString& branch, const QString& upstream);

private:
    QString m_path;
    git_repository* m_repo = nullptr;
    /// Whether this instance took a hold on libgit2, so the destructor knows
    /// whether to give it back.
    bool m_initialised = false;
};

} // namespace mole::test
