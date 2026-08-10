#include "tools/tasks/Commands.h"

#include "tools/tasks/ToolEnvironment.h"

#include "core/diagnostics/Diagnostics.h"
#include "core/duplicates/FindDuplicatesTask.h"
#include "core/duplicates/Strategies.h"
#include "core/index/IndexDatabase.h"
#include "core/index/ScanTask.h"
#include "core/rename/RenamePlan.h"
#include "core/rename/RenameTask.h"
#include "core/sync/SyncTask.h"
#include "core/tasks/DriveCheckTask.h"
#include "core/tasks/TaskManager.h"
#include "core/tasks/TransferTask.h"
#include "core/vfs/VfsManager.h"

#ifdef MOLE_HAVE_ARCHIVE
#include "plugins/archive/CompressTask.h"
#endif

#include <QCoreApplication>
#include <QEventLoop>
#include <QLocale>
#include <QTextStream>
#include <QTimer>

#include <atomic>
#include <csignal>

namespace mole::tools {
namespace {

    /// Set from the signal handler and read from the wait loop. Nothing else is
    /// safe to do in a handler, and a runner that cannot be interrupted is no
    /// use for driving a long transfer by hand.
    std::atomic_bool g_interrupted { false };

    void onInterrupt(int)
    {
        g_interrupted.store(true);
    }

    /// A very small argument reader.
    ///
    /// Written rather than taken from QCommandLineParser because the commands
    /// differ enough that each would need its own parser, and because the help
    /// below is worth writing by hand: a generated one lists options, and what a
    /// person needs is the four lines that show what a command is for.
    class Args
    {
    public:
        explicit Args(QStringList tokens)
            : m_tokens(std::move(tokens))
        {
        }

        /// The value of `--name`, consumed. Empty when it was not given.
        QString value(const QString& name, const QString& fallback = {})
        {
            const QStringList all = values(name);
            return all.isEmpty() ? fallback : all.last();
        }

        /// Every `--name value`, in order. Repeatable options are the honest
        /// shape for "copy these three files".
        QStringList values(const QString& name)
        {
            QStringList found;
            const QString flag = QStringLiteral("--") + name;
            for (qsizetype i = 0; i < m_tokens.size();) {
                if (m_tokens.at(i) != flag) {
                    ++i;
                    continue;
                }
                if (i + 1 < m_tokens.size()) {
                    found.append(m_tokens.at(i + 1));
                    m_tokens.remove(i, 2);
                } else {
                    m_missingValue = flag;
                    m_tokens.remove(i, 1);
                }
            }
            return found;
        }

        bool flag(const QString& name)
        {
            const qsizetype at = m_tokens.indexOf(QStringLiteral("--") + name);
            if (at < 0)
                return false;
            m_tokens.remove(at, 1);
            return true;
        }

        /// The first word that is not an option, removed. That is the command
        /// name, and taking it out is what leaves the command its own arguments.
        QString takeCommand()
        {
            for (qsizetype i = 0; i < m_tokens.size(); ++i) {
                if (m_tokens.at(i).startsWith(QLatin1String("--")))
                    continue;
                const QString word = m_tokens.at(i);
                m_tokens.remove(i, 1);
                return word;
            }
            return {};
        }

        /// What is left that is not an option: the file names.
        QStringList positional() const
        {
            QStringList out;
            for (const QString& token : m_tokens) {
                if (!token.startsWith(QLatin1String("--")))
                    out.append(token);
            }
            return out;
        }

        /// The first option nobody asked about, so a typo is an error rather
        /// than something silently ignored.
        QString strayOption() const
        {
            for (const QString& token : m_tokens) {
                if (token.startsWith(QLatin1String("--")))
                    return token;
            }
            return {};
        }

        QString missingValue() const { return m_missingValue; }

    private:
        QStringList m_tokens;
        QString m_missingValue;
    };

    /// Runs a task to the end, printing what it says about itself.
    ///
    /// The event loop is spun rather than the thread blocked: tasks report
    /// through queued invocations, so a runner that waited on a condition
    /// variable would print nothing until the end and could not be interrupted.
    int await(Task* task, TaskManager& tasks, QTextStream& out, bool quiet)
    {
        QEventLoop loop;
        QString lastStatus;

        QObject::connect(task, &Task::stateChanged, &loop, [&] {
            if (task->isFinished())
                loop.quit();
        });
        if (!quiet) {
            QObject::connect(task, &Task::statusTextChanged, &loop, [&] {
                const QString status = task->statusText();
                if (status != lastStatus && !status.isEmpty()) {
                    lastStatus = status;
                    out << "  " << status << Qt::endl;
                }
            });
        }

        // Polled rather than delivered, because a signal handler may do almost
        // nothing at all. 100 ms is imperceptible to the person pressing Ctrl-C
        // and irrelevant to a transfer.
        QTimer interruptCheck;
        interruptCheck.setInterval(100);
        QObject::connect(&interruptCheck, &QTimer::timeout, &loop, [&] {
            if (g_interrupted.load())
                task->requestCancel();
        });
        interruptCheck.start();

        tasks.submit(task);
        if (!task->isFinished())
            loop.exec();

        if (task->state() == Task::State::Cancelled) {
            out << (g_interrupted.load() ? "interrupted" : "cancelled") << Qt::endl;
            return g_interrupted.load() ? Interrupted : TaskFailed;
        }
        if (task->state() == Task::State::Failed) {
            out << "failed: " << task->error().message << Qt::endl;
            return TaskFailed;
        }
        return Ok;
    }

    /// Prints the lines a task collected about what it could not do, and turns
    /// them into the exit code. A task that ran to the end with three files
    /// failed is not a success, whatever its state says.
    int reportFailures(const QStringList& failures, QTextStream& err, int codeSoFar)
    {
        if (failures.isEmpty())
            return codeSoFar;
        for (const QString& line : failures)
            err << "  " << line << Qt::endl;
        return codeSoFar == Ok ? TaskFailed : codeSoFar;
    }

    /// Resolves a uri to the drive that serves it, or explains what is missing.
    FileSystemPtr resolve(ToolEnvironment& environment, const VfsUri& uri, QTextStream& err)
    {
        FileSystemPtr fs = environment.drives().resolve(uri);
        if (!fs) {
            err << "no drive is mounted for " << uri.toString() << Qt::endl << "mounted drives:" << Qt::endl;
            for (const QString& line : environment.mountSummary())
                err << "  " << line << Qt::endl;
        }
        return fs;
    }

    QString formattedSize(qint64 bytes)
    {
        return QLocale().formattedDataSize(bytes);
    }

    // ---- the commands ------------------------------------------------------

    int runTransfer(TransferTask::Mode mode, Args& args, ToolEnvironment& environment, QTextStream& out,
        QTextStream& err, bool quiet)
    {
        const QStringList sources = args.values(QStringLiteral("from"));
        const QString to = args.value(QStringLiteral("to"));
        const QString name = args.value(QStringLiteral("name"));
        const QString conflict = args.value(QStringLiteral("on-conflict"), QStringLiteral("fail"));

        if (sources.isEmpty() || to.isEmpty()) {
            err << "copy and move need --from <uri> (repeatable) and --to <directory uri>" << Qt::endl;
            return BadUsage;
        }

        TransferTask::Request request;
        request.mode = mode;
        request.targetName = name;
        if (conflict == QLatin1String("skip"))
            request.onConflict = TransferTask::Conflict::Skip;
        else if (conflict == QLatin1String("overwrite"))
            request.onConflict = TransferTask::Conflict::Overwrite;
        else if (conflict != QLatin1String("fail")) {
            err << "--on-conflict takes fail, skip or overwrite" << Qt::endl;
            return BadUsage;
        }

        for (const QString& source : sources) {
            const VfsUri uri = VfsUri::fromString(source);
            FileSystemPtr fs = resolve(environment, uri, err);
            if (!fs)
                return NoDrive;
            if (request.sourceFileSystem && request.sourceFileSystem != fs) {
                err << "every --from must be on the same drive" << Qt::endl;
                return BadUsage;
            }
            request.sourceFileSystem = fs;
            request.sources.append(uri);
        }

        request.targetDirectory = VfsUri::fromString(to);
        request.targetFileSystem = resolve(environment, request.targetDirectory, err);
        if (!request.targetFileSystem)
            return NoDrive;

        auto* task = new TransferTask(request);
        const int code = await(task, environment.tasks(), out, quiet);
        out << task->copiedCount() << " transferred, " << task->skippedCount() << " skipped, "
            << task->failedCount() << " failed" << Qt::endl;
        return reportFailures(task->failures(), err, code);
    }

    int runDelete(Args& args, ToolEnvironment& environment, QTextStream& out, QTextStream& err, bool quiet)
    {
        const QStringList targets = args.positional();
        if (targets.isEmpty()) {
            err << "delete needs at least one uri" << Qt::endl;
            return BadUsage;
        }

        FileSystemPtr fs;
        QList<VfsUri> uris;
        for (const QString& target : targets) {
            const VfsUri uri = VfsUri::fromString(target);
            FileSystemPtr owner = resolve(environment, uri, err);
            if (!owner)
                return NoDrive;
            if (fs && fs != owner) {
                err << "every target must be on the same drive" << Qt::endl;
                return BadUsage;
            }
            fs = owner;
            uris.append(uri);
        }

        auto* task = new DeleteTask(fs, uris);
        const int code = await(task, environment.tasks(), out, quiet);
        out << task->deletedCount() << " deleted" << Qt::endl;
        return reportFailures(task->failures(), err, code);
    }

    int runSync(Args& args, ToolEnvironment& environment, QTextStream& out, QTextStream& err, bool quiet)
    {
        const QString from = args.value(QStringLiteral("from"));
        const QString to = args.value(QStringLiteral("to"));
        if (from.isEmpty() || to.isEmpty()) {
            err << "sync needs --from <uri> and --to <uri>" << Qt::endl;
            return BadUsage;
        }

        SyncOptions options;
        options.mode
            = SyncOptions::modeFromString(args.value(QStringLiteral("mode"), QStringLiteral("update")));
        options.compare = SyncOptions::compareFromString(
            args.value(QStringLiteral("compare"), QStringLiteral("size-and-time")));
        options.includePatterns = args.values(QStringLiteral("include"));
        options.excludePatterns = args.values(QStringLiteral("exclude"));
        options.includeHidden = args.flag(QStringLiteral("hidden"));
        options.recursive = !args.flag(QStringLiteral("no-recursive"));
        options.skipNewer = !args.flag(QStringLiteral("no-skip-newer"));
        // A dry run is the default, and stays the default here. Anything that
        // can delete files should say what it would do before it does it.
        options.dryRun = !args.flag(QStringLiteral("apply"));

        const VfsUri source = VfsUri::fromString(from);
        const VfsUri target = VfsUri::fromString(to);
        FileSystemPtr sourceFs = resolve(environment, source, err);
        FileSystemPtr targetFs = sourceFs ? resolve(environment, target, err) : nullptr;
        if (!sourceFs || !targetFs)
            return NoDrive;

        auto* task = new SyncTask(sourceFs, source, targetFs, target, options);
        const int code = await(task, environment.tasks(), out, quiet);

        const SyncPlan plan = task->plan();
        for (const SyncPlan::Step& step : plan.steps()) {
            out << "  " << SyncPlan::actionLabel(step.action) << "  " << step.relativePath;
            if (!step.reason.isEmpty())
                out << "  (" << step.reason << ")";
            out << Qt::endl;
        }
        out << plan.steps().size() << " step(s), " << formattedSize(plan.bytesToTransfer());
        if (options.dryRun)
            out << " -- dry run, nothing was written. Add --apply." << Qt::endl;
        else
            out << ", " << task->appliedCount() << " applied" << Qt::endl;
        return reportFailures(task->failures(), err, code);
    }

    int runCompress(Args& args, ToolEnvironment& environment, QTextStream& out, QTextStream& err, bool quiet)
    {
#ifndef MOLE_HAVE_ARCHIVE
        Q_UNUSED(args);
        Q_UNUSED(environment);
        Q_UNUSED(out);
        Q_UNUSED(quiet);
        err << "this build has no archive support (libarchive was not found)" << Qt::endl;
        return BadUsage;
#else
        const QStringList sources = args.values(QStringLiteral("from"));
        const QString to = args.value(QStringLiteral("to"));
        if (sources.isEmpty() || to.isEmpty()) {
            err << "compress needs --from <uri> (repeatable) and --to <archive uri>" << Qt::endl;
            return BadUsage;
        }

        CompressTask::Request request;
        const QString format = args.value(QStringLiteral("format"), QStringLiteral("zip"));
        request.format = CompressTask::formatFromName(format);
        request.passphrase = args.value(QStringLiteral("password"));

        for (const QString& source : sources) {
            const VfsUri uri = VfsUri::fromString(source);
            FileSystemPtr fs = resolve(environment, uri, err);
            if (!fs)
                return NoDrive;
            if (request.sourceFileSystem && request.sourceFileSystem != fs) {
                err << "every --from must be on the same drive" << Qt::endl;
                return BadUsage;
            }
            request.sourceFileSystem = fs;
            request.sources.append(uri);
        }

        request.target = VfsUri::fromString(to);
        request.targetFileSystem = resolve(environment, request.target, err);
        if (!request.targetFileSystem)
            return NoDrive;

        auto* task = new CompressTask(request);
        const int code = await(task, environment.tasks(), out, quiet);
        out << task->packedCount() << " packed into " << request.target.toString() << Qt::endl;
        return reportFailures(task->failures(), err, code);
#endif
    }

    int runRename(Args& args, ToolEnvironment& environment, QTextStream& out, QTextStream& err, bool quiet)
    {
        const QString in = args.value(QStringLiteral("in"));
        if (in.isEmpty()) {
            err << "rename needs --in <directory uri>" << Qt::endl;
            return BadUsage;
        }

        QList<RenameRule> rules;
        if (const QString find = args.value(QStringLiteral("find")); !find.isEmpty()) {
            RenameRule rule;
            rule.kind = RenameRule::Kind::Replace;
            rule.find = find;
            rule.replaceWith = args.value(QStringLiteral("replace"));
            rule.useRegex = args.flag(QStringLiteral("regex"));
            rules.append(rule);
        }
        if (const QString style = args.value(QStringLiteral("case")); !style.isEmpty()) {
            RenameRule rule;
            rule.kind = RenameRule::Kind::Case;
            if (style == QLatin1String("upper"))
                rule.caseStyle = RenameRule::CaseStyle::Upper;
            else if (style == QLatin1String("lower"))
                rule.caseStyle = RenameRule::CaseStyle::Lower;
            else if (style == QLatin1String("title"))
                rule.caseStyle = RenameRule::CaseStyle::Title;
            else if (style == QLatin1String("sentence"))
                rule.caseStyle = RenameRule::CaseStyle::Sentence;
            else {
                err << "--case takes upper, lower, title or sentence" << Qt::endl;
                return BadUsage;
            }
            rules.append(rule);
        }
        const QString prefix = args.value(QStringLiteral("prefix"));
        const QString suffix = args.value(QStringLiteral("suffix"));
        if (!prefix.isEmpty() || !suffix.isEmpty()) {
            RenameRule rule;
            rule.kind = RenameRule::Kind::Affix;
            rule.prefix = prefix;
            rule.suffix = suffix;
            rules.append(rule);
        }
        if (const QString from = args.value(QStringLiteral("number-from")); !from.isEmpty()) {
            RenameRule rule;
            rule.kind = RenameRule::Kind::Number;
            rule.start = from.toInt();
            rule.step = 1;
            rules.append(rule);
        }

        if (rules.isEmpty()) {
            err << "rename needs at least one rule: --find/--replace, --case, "
                   "--prefix, --suffix or --number-from"
                << Qt::endl;
            return BadUsage;
        }

        const VfsUri directory = VfsUri::fromString(in);
        FileSystemPtr fs = resolve(environment, directory, err);
        if (!fs)
            return NoDrive;

        const Result<FileEntryList> listing = fs->list(directory, CancelToken());
        if (!listing.ok()) {
            err << "could not read " << in << ": " << listing.error().message << Qt::endl;
            return NoDrive;
        }

        QList<VfsUri> sources;
        QStringList existing;
        for (const FileEntry& entry : listing.value()) {
            existing.append(entry.name);
            if (!entry.isDir)
                sources.append(entry.uri);
        }
        if (sources.isEmpty()) {
            out << "nothing to rename in " << in << Qt::endl;
            return Ok;
        }

        // Keyed the way RenamePlan keys it -- by each source's own parent -- so
        // a name already taken is caught here rather than halfway through.
        QHash<QString, QStringList> existingByDirectory;
        existingByDirectory.insert(sources.first().parent().toString(), existing);
        const RenamePlan plan = RenamePlan::build(sources, rules, existingByDirectory);

        QList<RenamePlan::Entry> changed;
        for (const RenamePlan::Entry& entry : plan.entries()) {
            if (entry.isBlocked())
                err << "  " << entry.originalName << ": " << entry.problem << Qt::endl;
            else if (entry.changed()) {
                out << "  " << entry.originalName << "  ->  " << entry.newName << Qt::endl;
                changed.append(entry);
            }
        }

        if (!args.flag(QStringLiteral("apply"))) {
            out << changed.size() << " name(s) would change -- add --apply" << Qt::endl;
            return plan.blockedCount() > 0 ? TaskFailed : Ok;
        }
        if (!plan.canApply()) {
            err << "the plan cannot be applied as it stands" << Qt::endl;
            return TaskFailed;
        }

        auto* task = new RenameTask(&environment.drives(), changed);
        const int code = await(task, environment.tasks(), out, quiet);
        out << task->renamedCount() << " renamed" << Qt::endl;
        return reportFailures(task->failures(), err, code);
    }

    int runScan(Args& args, ToolEnvironment& environment, QTextStream& out, QTextStream& err, bool quiet)
    {
        const QStringList roots = args.positional();
        if (roots.size() != 1) {
            err << "scan takes exactly one uri" << Qt::endl;
            return BadUsage;
        }

        const VfsUri root = VfsUri::fromString(roots.first());
        FileSystemPtr fs = resolve(environment, root, err);
        if (!fs)
            return NoDrive;

        QString error;
        IndexDatabase* index = environment.index(&error);
        if (!index) {
            err << "could not open the index: " << error << Qt::endl;
            return NoDrive;
        }

        const QString label = args.value(QStringLiteral("label"), root.toString());
        auto* task = new ScanTask(fs, root, label, index);
        const int code = await(task, environment.tasks(), out, quiet);
        out << task->filesIndexed() << " indexed, " << task->skippedDirectories() << " directories skipped"
            << Qt::endl;
        return code;
    }

    int runDuplicates(
        Args& args, ToolEnvironment& environment, QTextStream& out, QTextStream& err, bool quiet)
    {
        const QString by = args.value(QStringLiteral("by"), QStringLiteral("content"));
        const qint64 minimum = args.value(QStringLiteral("min-size"), QStringLiteral("1")).toLongLong();
        const QStringList roots = args.positional();
        if (roots.isEmpty()) {
            err << "duplicates needs at least one uri" << Qt::endl;
            return BadUsage;
        }

        std::unique_ptr<IDuplicateStrategy> strategy;
        if (by == QLatin1String("size"))
            strategy = std::make_unique<SameSizeStrategy>();
        else if (by == QLatin1String("name"))
            strategy = std::make_unique<SameNameStrategy>();
        else if (by == QLatin1String("name+size"))
            strategy = std::make_unique<SameNameAndSizeStrategy>();
        else if (by == QLatin1String("content"))
            strategy = std::make_unique<SameContentStrategy>();
        else {
            err << "--by takes size, name, name+size or content" << Qt::endl;
            return BadUsage;
        }

        QList<VfsUri> uris;
        for (const QString& root : roots) {
            const VfsUri uri = VfsUri::fromString(root);
            if (!resolve(environment, uri, err))
                return NoDrive;
            uris.append(uri);
        }

        auto* task = new FindDuplicatesTask(&environment.drives(), uris, std::move(strategy));
        task->setMinimumSize(minimum);
        const int code = await(task, environment.tasks(), out, quiet);

        for (const DuplicateGroup& group : task->groups()) {
            out << formattedSize(group.reclaimable) << " reclaimable:" << Qt::endl;
            for (const FileEntry& file : group.files)
                out << "  " << file.uri.toString() << Qt::endl;
        }
        out << task->groups().size() << " group(s), " << formattedSize(task->reclaimableBytes())
            << " reclaimable" << Qt::endl;
        return code;
    }

    int runVerify(Args& args, ToolEnvironment& environment, QTextStream& out, QTextStream& err, bool quiet)
    {
        const QStringList roots = args.positional();
        if (roots.size() != 1) {
            err << "verify takes exactly one uri" << Qt::endl;
            return BadUsage;
        }

        const VfsUri root = VfsUri::fromString(roots.first());
        FileSystemPtr fs = resolve(environment, root, err);
        if (!fs)
            return NoDrive;

        // Building a backend proves nothing; this is the first real request,
        // which is what discovers a refused password or a certificate that does
        // not match. See DriveCheckTask.
        bool reachable = false;
        QString message;
        auto* task = new DriveCheckTask(root.toString(), fs, root);
        QObject::connect(task, &DriveCheckTask::checked, task, [&](bool ok, const QString& text) {
            reachable = ok;
            message = text;
        });

        const int code = await(task, environment.tasks(), out, quiet);
        out << message << Qt::endl;
        return reachable ? code : TaskFailed;
    }

    int runDrives(ToolEnvironment& environment, QTextStream& out)
    {
        out << "mounted:" << Qt::endl;
        for (const QString& line : environment.mountSummary())
            out << "  " << line << Qt::endl;
        return Ok;
    }

} // namespace

QString usageText()
{
    return QStringLiteral(R"(mole-tasks -- run any Mole task from a console, with no window.

  mole-tasks <command> [options]

Commands:
  copy        --from <uri>… --to <dir> [--name <n>] [--on-conflict fail|skip|overwrite]
  move        the same options; a move within one drive is a rename
  delete      <uri>…
  sync        --from <uri> --to <uri> [--mode update|mirror|fill]
              [--compare size-and-time|size|contents] [--include <glob>]…
              [--exclude <glob>]… [--hidden] [--no-recursive] [--no-skip-newer] [--apply]
  compress    --from <uri>… --to <archive> [--format zip|tar.gz|tar.xz|7z|xz] [--password <p>]
  rename      --in <dir> [--find <s> --replace <s> [--regex]] [--case upper|lower|title|sentence]
              [--prefix <s>] [--suffix <s>] [--number-from <n>] [--apply]
  scan        <uri> [--label <name>]
  duplicates  <uri>… [--by content|size|name|name+size] [--min-size <bytes>]
  verify      <uri>
  drives      what is mounted, and how to address it

Everywhere:
  --drive <name>    mount a drive from the configuration the application uses
  --mount <spec>    mount one described here: name=nas,type=sftp,host=…,user=…,
                    password=@ENV_VAR,root=/data -- a value written @NAME is read
                    from that environment variable, so it stays out of the
                    argument list and out of the shell history
  --log <what>      task, drive, net, curl or all -- the same names MOLE_LOG takes
  --quiet           print the result and nothing on the way

sync and rename work out what they would do and stop. Add --apply to carry it out.

Exit codes: 0 done, 1 something failed, 2 the command line is wrong,
3 a drive could not be reached, 130 interrupted.
)");
}

int runMoleTasks(
    const QStringList& arguments, ToolEnvironment& environment, QTextStream& out, QTextStream& err)
{
    Args args(arguments);

    // Before anything is built, so the log covers connecting as well as
    // copying -- which is where half the interesting failures are.
    const QStringList categories = args.values(QStringLiteral("log"));
    if (!categories.isEmpty()) {
        qputenv("MOLE_LOG", categories.join(QLatin1Char(',')).toLocal8Bit());
        const QStringList loud = diagnostics::applyEnvironment();
        if (!loud.isEmpty())
            err << "recording in detail: " << loud.join(QStringLiteral(", ")) << Qt::endl;
    }

    const bool quiet = args.flag(QStringLiteral("quiet"));
    const QStringList configured = args.values(QStringLiteral("drive"));
    const QStringList specs = args.values(QStringLiteral("mount"));

    const QString command = args.takeCommand();
    if (command.isEmpty() || command == QLatin1String("help")) {
        out << usageText();
        return command.isEmpty() ? BadUsage : Ok;
    }

    if (!args.missingValue().isEmpty()) {
        err << args.missingValue() << " needs a value" << Qt::endl;
        return BadUsage;
    }

    // Only when a drive beyond local disk was asked for: loading plugins costs
    // a directory scan and a handful of dlopen calls, and a copy between two
    // local paths needs none of it.
    if (!configured.isEmpty() || !specs.isEmpty())
        environment.loadPlugins();

    for (const QString& name : configured) {
        QString error;
        if (!environment.mountConfigured(name, &error)) {
            err << error << Qt::endl;
            for (const QString& problem : environment.pluginErrors())
                err << "  plugin: " << problem << Qt::endl;
            return NoDrive;
        }
    }
    for (const QString& spec : specs) {
        QString error;
        if (!environment.mountFromSpec(spec, &error)) {
            err << error << Qt::endl;
            for (const QString& problem : environment.pluginErrors())
                err << "  plugin: " << problem << Qt::endl;
            return NoDrive;
        }
    }
    if (!quiet && (!configured.isEmpty() || !specs.isEmpty())) {
        for (const QString& line : environment.mountSummary())
            out << "mounted " << line << Qt::endl;
    }

    // Ctrl-C asks the task to stop rather than killing the process, so a
    // half-written file is cleaned up the way a cancelled copy's is.
    std::signal(SIGINT, onInterrupt);
    g_interrupted.store(false);

    // Everything global has been taken out of `args` already, so what is left
    // belongs to the command.
    Args& commandArgs = args;

    int code = BadUsage;
    if (command == QLatin1String("copy"))
        code = runTransfer(TransferTask::Mode::Copy, commandArgs, environment, out, err, quiet);
    else if (command == QLatin1String("move"))
        code = runTransfer(TransferTask::Mode::Move, commandArgs, environment, out, err, quiet);
    else if (command == QLatin1String("delete"))
        code = runDelete(commandArgs, environment, out, err, quiet);
    else if (command == QLatin1String("sync"))
        code = runSync(commandArgs, environment, out, err, quiet);
    else if (command == QLatin1String("compress"))
        code = runCompress(commandArgs, environment, out, err, quiet);
    else if (command == QLatin1String("rename"))
        code = runRename(commandArgs, environment, out, err, quiet);
    else if (command == QLatin1String("scan"))
        code = runScan(commandArgs, environment, out, err, quiet);
    else if (command == QLatin1String("duplicates"))
        code = runDuplicates(commandArgs, environment, out, err, quiet);
    else if (command == QLatin1String("verify"))
        code = runVerify(commandArgs, environment, out, err, quiet);
    else if (command == QLatin1String("drives"))
        code = runDrives(environment, out);
    else {
        err << "no such command: " << command << Qt::endl << Qt::endl << usageText();
        return BadUsage;
    }

    // Asked after the command has read what it wanted, because that is when an
    // option that was given nothing is finally known about. A command that
    // reported "you forgot --to" would be describing a different mistake.
    if (!commandArgs.missingValue().isEmpty()) {
        err << commandArgs.missingValue() << " needs a value" << Qt::endl;
        return BadUsage;
    }

    if (const QString stray = commandArgs.strayOption(); !stray.isEmpty() && code == Ok) {
        err << stray << " means nothing to " << command << Qt::endl;
        return BadUsage;
    }
    return code;
}

} // namespace mole::tools
