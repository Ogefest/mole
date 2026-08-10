#pragma once

#include <QProcess>
#include <QString>

#include <functional>

namespace mole::test {

/// Another copy of this test binary, started so that it can be killed.
///
/// `SIGKILL` is the one failure that cannot be simulated. No destructor runs, no
/// error path runs, nothing gets a chance to tidy up — which is exactly why it
/// is worth testing and exactly why it cannot be faked from inside a process
/// that intends to keep running. So a real process does the work and a real
/// signal stops it.
///
/// The victim is this same binary, re-run with one test function and an
/// environment variable naming what to work on. A test using it has two halves
/// in one function:
///
/// ```
/// void TestSomething::aThingKilledMidWay()
/// {
///     if (Victim::isThisProcess()) {
///         doTheWorkForever(Victim::instruction());   // never returns
///         return;
///     }
///     Victim victim(QStringLiteral("aThingKilledMidWay"), path);
///     QVERIFY(victim.started());
///     QVERIFY(victim.waitUntil([&] { return somethingHasBegun(); }));
///     victim.kill();
///     // ... and now look at what is left
/// }
/// ```
///
/// **Waiting is on a condition, never on a clock.** A test that sleeps for
/// 200 ms passes on one machine and fails on another, and the kill has to land
/// while the work is in flight — too early and there is nothing to interrupt,
/// too late and there is nothing left to interrupt.
class Victim
{
public:
    /// True in the process that was started to be killed. False in the one doing
    /// the killing, which is where the assertions live.
    static bool isThisProcess();

    /// What the parent handed over — usually a path to work on.
    static QString instruction();

    Victim(const QString& testFunction, const QString& instruction);
    ~Victim();

    Victim(const Victim&) = delete;
    Victim& operator=(const Victim&) = delete;

    bool started() const { return m_started; }

    /// Polls until `condition` holds, the victim exits, or the attempts run out.
    /// False means it never happened, which is a failed test rather than a
    /// reason to carry on.
    bool waitUntil(const std::function<bool()>& condition, int attempts = 400, int gapMs = 25);

    /// SIGKILL, and then reaped. Safe to call more than once.
    void kill();

    /// Whatever the victim printed before it died. Worth having in a failure
    /// message: a victim that fell over for its own reasons looks exactly like
    /// one that was killed at the wrong moment.
    QString transcript();

private:
    QProcess m_process;
    bool m_started = false;
    QString m_transcript;
};

} // namespace mole::test
