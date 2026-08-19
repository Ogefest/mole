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
