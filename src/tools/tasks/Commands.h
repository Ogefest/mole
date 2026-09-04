#pragma once

#include <QStringList>

class QTextStream;

namespace mole::tools {

class ToolEnvironment;

/// What the process exits with. A shell script driving a transfer under
/// `tc netem` has to be able to tell "the copy failed" from "I typed the
/// command wrong", and both from "the drive was never reachable" -- one exit
/// code for all three would make the runner useless in a loop.
enum ExitCode {
    Ok = 0,
    TaskFailed = 1, ///< the work ran and something in it did not land
    BadUsage = 2, ///< nothing ran: the command line is wrong
    NoDrive = 3, ///< nothing ran: a drive could not be reached or configured
    Interrupted = 130 ///< Ctrl-C, by the convention every shell already knows
};

/// Runs one command line, without the process's own name.
///
/// **`out` is the result and nothing else.** Progress, the list of mounted
/// drives, warnings, the usage text when it accompanies a mistake, and every
/// error go to `err`, so `mole-tasks duplicates … > list` is a file of
/// duplicates rather than a file of duplicates with status lines through it.
/// Nothing documented this before and the two streams interleaved at random.
int runMoleTasks(
    const QStringList& arguments, ToolEnvironment& environment, QTextStream& out, QTextStream& err);

/// Asks the run in progress to stop, exactly as Ctrl-C does.
///
/// The SIGINT handler sets a flag and no more -- almost nothing is safe to do in
/// one -- and the wait loop polls it. A caller that is not a signal handler goes
/// through here instead and the running task is cancelled at once, which is what
/// makes "interrupted" something a test can assert on rather than race with.
void interruptMoleTasks();

/// What `mole-tasks` with no arguments prints.
QString usageText();

} // namespace mole::tools
