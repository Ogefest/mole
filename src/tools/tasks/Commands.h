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

/// Runs one command line, without the process's own name. Everything it prints
/// goes to `out` and `err` so a test can read it back.
int runMoleTasks(
    const QStringList& arguments, ToolEnvironment& environment, QTextStream& out, QTextStream& err);

/// What `mole-tasks` with no arguments prints.
QString usageText();

} // namespace mole::tools
