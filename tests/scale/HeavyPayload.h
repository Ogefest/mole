#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <mutex>
#include <thread>

class QIODevice;

namespace mole::test {

/// A payload too big to hold, and still verified byte for byte.
///
/// Ten gigabytes cannot be kept in memory to compare against, and hashing it
/// twice costs more than the transfer on a fast link. So the content is a
/// function of where it is: one megabyte of fixed pseudo-random bytes, repeated,
/// with each block stamped with its own index in the first eight bytes.
///
/// That is a real byte-for-byte check rather than a sample. Every byte is
/// compared against what belongs at that offset, and stamping the blocks is what
/// catches the failure a repeating pattern would otherwise hide: a copy that
/// duplicated, dropped or reordered a block would match a plain repetition
/// perfectly.
class HeavyPayload
{
public:
    static constexpr qint64 kBlockSize = 1024 * 1024;

    /// Fills `out` with the block that belongs at `blockIndex`.
    static void block(QByteArray& out, qint64 blockIndex);

    /// Writes `bytes` of payload to `device`, in blocks. Returns false and fills
    /// `errorOut` on the first short write -- at this size, "it failed
    /// somewhere" is not a diagnosis.
    static bool writeTo(QIODevice& device, qint64 bytes, QString* errorOut);

    /// Reads `device` to the end and checks every byte against what belongs
    /// there. `errorOut` names the offset of the first byte that is wrong, or
    /// says how far short the stream stopped.
    static bool verify(QIODevice& device, qint64 expectedBytes, QString* errorOut);
};

/// The smallest transfer still worth calling heavy.
///
/// Below this the case stops being about what this tier is about. `ResourceWatch`
/// is asked whether a copy needed room for a chunk or room for the whole file, and
/// the allowance that question is asked against never drops under 64 MiB -- so a
/// payload anywhere near it could not tell a copy that streams from one that
/// stages. Four times that, which is also the payload this binary uses when nobody
/// asks for more.
constexpr qint64 kSmallestHeavyPayload = 256LL * 1024 * 1024;

/// How much of `wanted` to send to a destination with `capacity` bytes free that
/// will accept at most `mostInOneTransfer` bytes in a single request.
///
/// **The tier's payload, or as much of it as the destination can hold, rather than
/// one figure for all four.** The WebDAV and FTP roots on the test machine are on a
/// four-gigabyte disk on purpose -- that is what makes "the destination filled up" a
/// condition a test can create in seconds instead of faking -- and the tier's
/// default payload is ten gibibytes, also on purpose. Both decisions are right, they
/// are incompatible, and nothing had ever put them together: the first real use of
/// `make release` refused itself, correctly, on the two destinations that skipped.
/// See MOLE-320.
///
/// What this tier exists to prove is the *ratio* between a transfer and the
/// temporary space it needs, and a gibibyte proves that as well as ten does. What it
/// does not need is to be one number.
///
/// **Half the room, at most.** That is the headroom the skip it replaces used to
/// insist on, and it is not caution for its own sake: a destination this tier filled
/// to the brim would take every other suite on that machine down with it. A capacity
/// of zero means nobody could ask, and then the full payload goes -- an absent
/// control channel must not quietly shrink the tier.
///
/// **`mostInOneTransfer` is a second ceiling and it is not about space at all.**
/// Apache 2.4 refuses a request body over 1 GiB with a 413, from its own default
/// rather than from anything in its configuration -- measured against the test
/// machine on 2026-09-01, where exactly 1073741824 bytes is accepted and one byte
/// more is refused before the body is read. A WebDAV upload is one request, because
/// the protocol has no ranged PUT, so that is a ceiling on the file and not on a
/// chunk of it. Room and what one request may carry are different questions and the
/// smaller answer wins. Zero means no such limit is known. See MOLE-320.
[[nodiscard]] qint64 heavyPayloadFor(qint64 wanted, qint64 capacity, qint64 mostInOneTransfer = 0);

/// What a transfer costs in things other than time.
///
/// A copy of a hundred-gigabyte file must not need a hundred gigabytes of
/// temporary space, must not grow without bound, and must give its file
/// descriptors back. None of those is visible in the result of the copy: it
/// succeeds either way, which is exactly how staging survived until it became a
/// wall. See ADR-0014.
///
/// Sampled on a timer rather than triggered by an event, because there is no
/// event -- this is a measurement of a continuous quantity, not a wait for a
/// state. It is the one place in this suite where a clock is the right tool.
class ResourceWatch
{
public:
    /// Takes the baseline and starts sampling.
    ResourceWatch();
    ~ResourceWatch();

    ResourceWatch(const ResourceWatch&) = delete;
    ResourceWatch& operator=(const ResourceWatch&) = delete;

    /// Stops sampling. Safe to call twice; the destructor does it.
    void stop();

    /// The most bytes seen in temporary files that were not there when this
    /// started. This is the number that catches staging.
    qint64 peakScratchBytes() const { return m_peakScratch.load(); }
    /// Which entry was the largest when that peak was taken.
    ///
    /// A bare number says a transfer staged and not what did the staging, which
    /// is the first thing anybody looking at a red line here has to find out --
    /// and finding it out meant reproducing the run with a shell watching /tmp.
    QString largestScratchEntry() const;
    /// How much the resident set grew above the baseline, at its worst.
    qint64 peakResidentGrowthBytes() const { return m_peakRss.load(); }
    /// Open file descriptors now, and how many there were at the start.
    static int openDescriptors();
    int baselineDescriptors() const { return m_baselineFds; }

    /// One line for a log: what the transfer cost besides time.
    QString summary() const;

private:
    void sample();

    std::atomic_bool m_running { true };
    std::atomic<qint64> m_peakScratch { 0 };
    mutable std::mutex m_namesGuard;
    QString m_largestEntry;
    std::atomic<qint64> m_peakRss { 0 };
    qint64 m_baselineRss = 0;
    int m_baselineFds = 0;
    QString m_tempPath;
    QByteArray m_baselineEntries;
    std::thread m_sampler;
};

} // namespace mole::test
