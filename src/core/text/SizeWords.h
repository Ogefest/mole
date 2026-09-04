#pragma once

#include <QString>

#include <cstdint>

namespace mole {

/// A number of bytes, in words: "1.5 GiB", "834 KiB", "12 bytes".
///
/// **One formatter, because there were two and they disagreed about what a
/// gigabyte is called.** Nineteen files asked `QLocale::formattedDataSize()` --
/// IEC units, digits grouped by the locale -- for the task strip, the sidebar,
/// transfers, sync, duplicates, reports, sets and the previews. Four asked
/// `FileListModel::formatSize()`, which divides by 1024 and then labels the
/// result kB/MB/GB with no locale, for the listing itself, the analysis view, the
/// search results and the breakdown. So **the same file was "1.5 GB" in the pane
/// and "1.5 GiB" in the strip beneath it** -- and SearchFeatures.cpp's own
/// comment said "a file manager showing GiB everywhere else" while the listing
/// it sits above showed GB.
///
/// IEC, and grouped by the locale, which is what the nineteen already did and
/// what the divisor in the other four always meant: dividing by 1024 and calling
/// the answer a kB is the one combination that is simply wrong.
///
/// In core so that a plugin can reach it -- the listing, the analysis tab and the
/// duplicate scan are in three different layers. See MOLE-403.
QString sizeInWords(qint64 bytes);

/// The same, per second: "12.4 MiB/s". Empty for a negative rate.
QString rateInWords(double bytesPerSecond);

} // namespace mole
