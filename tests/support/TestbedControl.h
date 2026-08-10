#pragma once

#include <QString>
#include <QStringList>

namespace mole::test {

/// Doing something to the test server while a test is watching.
///
/// Reading a file from a server that is behaving is a small part of what a file
/// manager has to survive. The rest is what happens when the connection dies,
/// the disk fills or the network goes slow -- and none of that can be asserted
/// without being able to cause it on purpose.
///
/// **Absent by default.** Nothing here does anything unless `MOLE_TEST_CONTROL`
/// names the command to reach the machine with, so a suite running on somebody's
/// own machine cannot start stopping services on anything. A test that needs it
/// asks `isAvailable()` and skips itself when it is not there, the same way the
/// live backend suites already do.
///
/// `MOLE_TEST_CONTROL` holds a command prefix, typically
/// `ssh user@machine sudo mole-control`. Arguments are appended to it.
class TestbedControl
{
public:
    /// Whether a control channel was named. False on any ordinary machine.
    static bool isAvailable();

    /// Runs one command and returns what the machine said it did.
    ///
    /// The answer matters as much as the effect: a test that fails after
    /// interfering has to be able to say what it interfered with, and "something
    /// was done to the server" is not something anybody can act on.
    static QString run(const QStringList& arguments, int timeoutMs = 60000);

    /// Undoes everything: no ballast on the disk, no netem, every server up.
    /// Safe to call when nothing was done.
    static QString restore();
};

} // namespace mole::test
