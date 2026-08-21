#pragma once

#include "core/platform/HostPlatform.h"

#include <QString>

namespace mole {

/// What a destination will accept in a file name.
///
/// Nothing between a name being chosen and a file being written used to ask
/// this. On Linux almost everything is accepted, so the gap never showed -- and
/// the places that generate names are exactly the places that meet a name the
/// destination refuses: extracting an archive, and copying off a remote drive
/// where `really?.txt` and `a:b.txt` are perfectly legal.
///
/// A rule set rather than a platform, because the destination is what knows. A
/// FAT-formatted stick on Linux is stricter than the disk it is plugged into,
/// and an S3 bucket is stricter than either about nothing at all.
struct NameRules
{
    /// Refused outright, one character each.
    QString forbiddenCharacters;
    /// Whether 0x00-0x1F are refused. A newline in a name is legal on Linux and
    /// is in the awkward-names suite for that reason.
    bool refusesControlCharacters = false;
    /// Windows strips a trailing dot or space silently, so a file written as
    /// "report." arrives as "report" and the caller's next read misses it.
    bool refusesTrailingDotOrSpace = false;
    /// CON, PRN, AUX, NUL, COM1-9, LPT1-9 -- in any case, with or without an
    /// extension, because "nul.txt" is the device too.
    bool refusesReservedDeviceNames = false;
    /// Characters, not bytes. Zero means no limit this layer knows about.
    int maximumLength = 0;

    /// What the platform's usual filesystem accepts. Posix and macOS refuse
    /// almost nothing; Windows refuses a good deal.
    static NameRules forPlatform(HostPlatform platform = hostPlatform());
};

/// Whether a name may be used, and what to do about it when it may not.
struct NameVerdict
{
    bool accepted = true;
    /// One sentence naming what is wrong, for a person. Empty when accepted.
    QString reason;
    /// A name the rules would accept, for an interface to *offer*. Never applied
    /// on its own: a file that arrives under a name nobody chose is harder to
    /// find later than one that did not arrive.
    QString suggestion;

    bool isRejected() const { return !accepted; }
};

/// Pure, and the whole point of it being pure is that the Windows rule set can
/// be checked on a Linux machine.
///
/// Refuses the four names no filesystem can hold whatever the rules say: empty,
/// "." and "..", and anything containing a separator or a null.
NameVerdict checkName(const QString& name, const NameRules& rules);

} // namespace mole
