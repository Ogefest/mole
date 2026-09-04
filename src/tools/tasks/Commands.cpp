#include "tools/tasks/Commands.h"

#include "sdk/ScanReaders.h"
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
#include "core/text/SizeWords.h"
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
#include <optional>

namespace mole::tools {
namespace {

    /// Set from the signal handler and read from the wait loop. Nothing else is
    /// safe to do in a handler, and a runner that cannot be interrupted is no
    /// use for driving a long transfer by hand.
    std::atomic_bool g_interrupted { false };

    /// The task the wait loop is sitting on, so an interrupt that did not come
    /// from a signal handler can stop it now rather than at the next poll. Only
    /// ever set and cleared by await(); requestCancel() sets one atomic flag, so
    /// calling it from anywhere is safe.
    std::atomic<Task*> g_running { nullptr };

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

        /// The command word, removed. Every option that applies everywhere has
        /// already been taken out, so the command is whatever is left at the
        /// front -- and an option there is a mistake rather than a command.
        ///
        /// It used to be "the first token that is not an option", which read the
        /// *value* of a command option placed too early as the command:
        /// `mole-tasks --to /x copy --from y` answered "no such command: /x".
        QString takeCommand()
        {
            if (m_tokens.isEmpty())
                return {};
            if (m_tokens.first().startsWith(QLatin1String("--"))) {
                m_optionBeforeCommand = m_tokens.first();
                return {};
            }
            return m_tokens.takeFirst();
        }

        /// The option that was found where the command should have been, if any.
        QString optionBeforeCommand() const { return m_optionBeforeCommand; }

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
        QString m_optionBeforeCommand;
    };

    /// Runs a task to the end, printing what it says about itself.
    ///
    /// The event loop is spun rather than the thread blocked: tasks report
    /// through queued invocations, so a runner that waited on a condition
    /// variable would print nothing until the end and could not be interrupted.
    int await(Task* task, TaskManager& tasks, QTextStream& err, bool quiet)
    {
        QEventLoop loop;
        QString lastStatus;

        QObject::connect(task, &Task::stateChanged, &loop, [&] {
            if (task->isFinished())
                loop.quit();
        });
        if (!quiet) {
            // On stderr, with everything else that is not the result: a status
            // line landing in the middle of a redirected list of duplicates is
            // a line somebody's next command has to filter back out.
            QObject::connect(task, &Task::statusTextChanged, &loop, [&] {
                const QString status = task->statusText();
                if (status != lastStatus && !status.isEmpty()) {
                    lastStatus = status;
                    err << "  " << status << Qt::endl;
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

        g_running.store(task);
        tasks.submit(task);
        if (!task->isFinished())
            loop.exec();
        g_running.store(nullptr);

        if (task->state() == Task::State::Cancelled) {
            err << (g_interrupted.load() ? "interrupted" : "cancelled") << Qt::endl;
            return g_interrupted.load() ? Interrupted : TaskFailed;
        }
        if (task->state() == Task::State::Failed) {
            err << "failed: " << task->error().message << Qt::endl;
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
        return sizeInWords(bytes);
    }

    /// Says what an option would have accepted, and returns BadUsage.
    ///
    /// Every option a person types is read strictly. Three of them used to go
    /// through parsers written to be forgiving for a stored file and a picker,
    /// so `--mode miror --apply` ran an update sync and `--format tar.bz2` wrote
    /// a zip called `x.tar.bz2`. ADR-0028: "Anything that can delete files does
    /// not do it on the strength of a typo." See MOLE-391.
    int refuseValue(QTextStream& err, const QString& option, const QString& given, const QStringList& accepts)
    {
        err << "--" << option << " does not take '" << given << "'. It takes "
            << accepts.join(QStringLiteral(", ")) << Qt::endl;
        return BadUsage;
    }

    /// A whole number, or nothing. `toLongLong()` with no `ok` answers 0 for
    /// "lots" and for "1O", and 0 is a bound that lets everything through.
    std::optional<qint64> wholeNumber(const QString& text)
    {
        bool ok = false;
        const qint64 value = text.trimmed().toLongLong(&ok);
        return ok ? std::optional<qint64>(value) : std::nullopt;
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
        const int code = await(task, environment.tasks(), err, quiet);
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
        const int code = await(task, environment.tasks(), err, quiet);
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
        if (const QString mode = args.value(QStringLiteral("mode")); !mode.isEmpty()) {
            const std::optional<SyncOptions::Mode> wanted = SyncOptions::modeIfKnown(mode);
            if (!wanted)
                return refuseValue(err, QStringLiteral("mode"), mode, SyncOptions::modeNames());
            options.mode = *wanted;
        }
        if (const QString compare = args.value(QStringLiteral("compare")); !compare.isEmpty()) {
            const std::optional<SyncOptions::Compare> wanted = SyncOptions::compareIfKnown(compare);
            if (!wanted)
                return refuseValue(err, QStringLiteral("compare"), compare, SyncOptions::compareNames());
            options.compare = *wanted;
        }
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
        const int code = await(task, environment.tasks(), err, quiet);

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
        if (const QString format = args.value(QStringLiteral("format")); !format.isEmpty()) {
            const std::optional<CompressTask::Format> wanted = CompressTask::formatIfKnown(format);
            if (!wanted)
                return refuseValue(err, QStringLiteral("format"), format, CompressTask::formatNames());
            request.format = *wanted;
        }

        // Named, never typed. ADR-0028 says secrets never appear in an argument,
        // and an argument is in `ps`, in the shell history and in any CI log
        // that echoes the command before running it.
        if (const QString password = args.value(QStringLiteral("password")); !password.isEmpty()) {
            QString problem;
            request.passphrase = secretFromEnvironment(password, &problem);
            if (!problem.isEmpty()) {
                err << "--password: " << problem << Qt::endl;
                return BadUsage;
            }
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

        request.target = VfsUri::fromString(to);
        request.targetFileSystem = resolve(environment, request.target, err);
        if (!request.targetFileSystem)
            return NoDrive;

        auto* task = new CompressTask(request);
        const int code = await(task, environment.tasks(), err, quiet);
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
            const std::optional<qint64> start = wholeNumber(from);
            if (!start) {
                err << "--number-from takes a whole number, not '" << from << "'" << Qt::endl;
                return BadUsage;
            }
            RenameRule rule;
            rule.kind = RenameRule::Kind::Number;
            rule.start = static_cast<int>(*start);
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
        const int code = await(task, environment.tasks(), err, quiet);
        out << task->renamedCount() << " renamed" << Qt::endl;
        return reportFailures(task->failures(), err, code);
    }

    int runScan(Args& args, ToolEnvironment& environment, QTextStream& out, QTextStream& err, bool quiet)
    {
        // Every option first. positional() is "whatever does not begin with
        // --", so an option's *value* is one of them until the option has been
        // taken out -- `scan <uri> --label fixture` counted two roots and was
        // refused as "scan takes exactly one uri".
        const QString label = args.value(QStringLiteral("label"));
        ScanOptions options;
        options.incremental = args.flag(QStringLiteral("incremental"));
        options.archives = args.flag(QStringLiteral("archives"));

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

        auto* task = new ScanTask(fs, root, label.isEmpty() ? root.toString() : label, index);

        // Whichever backend mounts a zip is a plugin, so a scan asked to go
        // inside one has to have loaded them. Not otherwise: a scan of a local
        // tree needs none of it.
        if (options.archives)
            environment.loadPlugins();
        // Through the same call the window uses rather than by hand, which is
        // the whole of ADR-0056: a scan built in a second place is a scan that
        // quietly indexes less. Metadata is not offered because this binary
        // registers no metadata readers -- factReaderFor() answers null and
        // applyScanOptions() asks for nothing, which is the honest outcome.
        applyScanOptions(*task, options, environment.services(), fs, root);
        const int code = await(task, environment.tasks(), err, quiet);
        out << task->filesIndexed() << " indexed, " << task->skippedDirectories() << " directories skipped";
        if (options.archives)
            out << ", " << task->containedEntries() << " inside containers";
        out << Qt::endl;
        return code;
    }

    int runDuplicates(
        Args& args, ToolEnvironment& environment, QTextStream& out, QTextStream& err, bool quiet)
    {
        const QString by = args.value(QStringLiteral("by"), QStringLiteral("content"));
        const QString minimumText = args.value(QStringLiteral("min-size"), QStringLiteral("1"));
        const std::optional<qint64> minimum = wholeNumber(minimumText);
        if (!minimum) {
            err << "--min-size takes a number of bytes, not '" << minimumText << "'" << Qt::endl;
            return BadUsage;
        }
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
        task->setMinimumSize(*minimum);
        const int code = await(task, environment.tasks(), err, quiet);

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

        const int code = await(task, environment.tasks(), err, quiet);
        out << message << Qt::endl;
        return reachable ? code : TaskFailed;
    }

    int runDrives(Args& args, ToolEnvironment& environment, QTextStream& out)
    {
        out << "mounted:" << Qt::endl;
        for (const QString& line : environment.mountSummary())
            out << "  " << line << Qt::endl;

        // The first question of any report about a package -- was the network
        // backend found at all -- and until now it could only be answered by
        // asking for a drive and reading a failure. Plugin errors printed only
        // when a mount failed, so a build with no sftp looked exactly like a
        // drive name typed wrong.
        if (!args.flag(QStringLiteral("plugins")))
            return Ok;

        environment.loadPlugins();
        out << "plugins looked for in:" << Qt::endl;
        for (const QString& path : environment.pluginSearchPaths())
            out << "  " << path << Qt::endl;

        const QStringList loaded = environment.loadedPlugins();
        out << (loaded.isEmpty() ? QStringLiteral("nothing loaded") : QStringLiteral("loaded:")) << Qt::endl;
        for (const QString& line : loaded)
            out << "  " << line << Qt::endl;

        const QStringList problems = environment.pluginErrors();
        if (!problems.isEmpty()) {
            out << "problems:" << Qt::endl;
            for (const QString& line : problems)
                out << "  " << line << Qt::endl;
        }

        // Kept apart from the problems. This binary has nowhere to put a feature,
        // a preview, a reader or a thumbnailer, and saying so is not the same as
        // saying a plugin got something wrong.
        const QStringList notes = environment.pluginNotes();
        if (!notes.isEmpty()) {
            out << "not taken here:" << Qt::endl;
            for (const QString& line : notes)
                out << "  " << line << Qt::endl;
        }
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
  compress    --from <uri>… --to <archive> [--format zip|tar.gz|tar.xz|7z|xz] [--password @ENV_VAR]
  rename      --in <dir> [--find <s> --replace <s> [--regex]] [--case upper|lower|title|sentence]
              [--prefix <s>] [--suffix <s>] [--number-from <n>] [--apply]
  scan        <uri> [--label <name>] [--incremental] [--archives]
  duplicates  <uri>… [--by content|size|name|name+size] [--min-size <bytes>]
  verify      <uri>
  drives      what is mounted, and how to address it [--plugins]

Everywhere:
  --drive <name>    mount a drive from the configuration the application uses
  --mount <spec>    mount one described here: name=nas,type=sftp,host=…,user=…,
                    password=@ENV_VAR,root=/data -- a value written @NAME is read
                    from that environment variable, so it stays out of the
                    argument list and out of the shell history
  --log <what>      task, drive, net, curl or all -- the same names MOLE_LOG takes
  --quiet           print the result and nothing on the way
  --help            this text
  --version         which Mole this is

Options come after the command word.

sync and rename work out what they would do and stop. Add --apply to carry it out.

A secret is never typed as an argument: write @NAME and put the value in that
environment variable. An argument is visible in ps, in the shell history, and in
any log that echoes the command before running it.

Standard output is the result -- the copied count, the plan, the list of
duplicates. Progress, warnings and every error go to standard error, so
redirecting the result gives a file with nothing else in it.

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

    // Answered before anything else and on stdout, because both are the result:
    // "which build is this" is the first question of every bug report, and this
    // is the binary that runs where there is no window to ask. Both used to exit
    // 2 -- --version was an unknown option and fell through to the usage text.
    const bool wantsHelp = args.flag(QStringLiteral("help"));
    if (args.flag(QStringLiteral("version"))) {
        out << QCoreApplication::applicationName() << ' ' << QCoreApplication::applicationVersion()
            << Qt::endl;
        return Ok;
    }
    if (wantsHelp) {
        out << usageText();
        return Ok;
    }

    const QStringList configured = args.values(QStringLiteral("drive"));
    const QStringList specs = args.values(QStringLiteral("mount"));

    const QString command = args.takeCommand();
    if (command == QLatin1String("help")) {
        out << usageText();
        return Ok;
    }
    if (!args.optionBeforeCommand().isEmpty()) {
        err << args.optionBeforeCommand() << " comes after the command word, not before it" << Qt::endl
            << Qt::endl
            << usageText();
        return BadUsage;
    }
    if (command.isEmpty()) {
        err << usageText();
        return BadUsage;
    }

    if (!args.missingValue().isEmpty()) {
        err << args.missingValue() << " needs a value" << Qt::endl;
        return BadUsage;
    }

    // Before anything is mounted. Connecting to a drive that is not answering is
    // one of the longer things this binary does, and Ctrl-C during it used to
    // kill the process by signal rather than end the run with 130.
    std::signal(SIGINT, onInterrupt);
    g_interrupted.store(false);

    // Only when a drive beyond local disk was asked for: loading plugins costs
    // a directory scan and a handful of dlopen calls, and a copy between two
    // local paths needs none of it. `drives --plugins` and `scan --archives`
    // ask for them themselves.
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
            err << "mounted " << line << Qt::endl;
    }

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
        code = runDrives(commandArgs, environment, out);
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

    // Said whatever the run did. It used to be said only on a clean run, so the
    // one case where a typo is the likeliest explanation -- the command failed
    // and an option it never read is sitting there -- was the case that kept
    // quiet about it. The command's own code still wins: "the copy failed" is a
    // more useful answer than "you also mistyped an option".
    if (const QString stray = commandArgs.strayOption(); !stray.isEmpty()) {
        err << stray << " means nothing to " << command << Qt::endl;
        return code == Ok ? BadUsage : code;
    }
    return code;
}

void interruptMoleTasks()
{
    g_interrupted.store(true);
    // Straight through rather than waiting for the poll: requestCancel() sets
    // one atomic flag, so it is safe from anywhere that is not a signal handler
    // -- and a caller that is not one deserves an answer now.
    if (Task* task = g_running.load())
        task->requestCancel();
}

} // namespace mole::tools
