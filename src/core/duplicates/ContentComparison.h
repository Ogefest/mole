#pragma once

#include "core/duplicates/DuplicateStrategy.h"
#include "core/vfs/IFileSystem.h"

#include <QList>

namespace mole {

/// Splits files into groups whose contents are identical, byte for byte.
///
/// This is what settles the last stage of a content scan, in place of a hash of
/// each file. Two properties matter and neither is available from a digest:
///
/// **It is exact.** A hash says two files are *probably* the same, and the next
/// thing that happens to a group is that all but one of it is deleted. SHA-256
/// makes that probability vanishingly small; a fast hash does not, and one that
/// is not collision-resistant can be made to collide by whoever writes the files.
/// A comparison has no probability in it at all.
///
/// **It costs less.** Each file is read exactly once, as it is with a hash, but
/// the work per byte is a memcmp rather than a digest -- two orders of magnitude
/// cheaper, which on any modern disk is the difference between waiting for the
/// processor and waiting for the storage. Files that differ stop at the first
/// chunk that differs, where a hash always reads to the end.
///
/// **Memory is bounded and does not grow with the file.** Files are read in
/// lockstep, a chunk at a time, and only the chunk is held -- so a group of
/// hundred-gigabyte disk images costs the same as a group of documents. No more
/// than kMaxOpenAtOnce files are read together, so a bucket of ten thousand
/// identical files cannot exhaust memory or file descriptors either; anything
/// larger is compared in slices and the slices are merged.
///
/// Every group is returned, including the ones left holding a single file: a
/// caller working through slices needs those to merge, and it is the caller that
/// decides a lone file is not a duplicate. A file that cannot be opened is left
/// out entirely -- an unreadable file is not a match for every other unreadable
/// file.
QList<QList<FileEntry>> partitionByContents(
    const QList<FileEntry>& files, const DriveLookup& driveFor, const CancelToken& cancel);

/// How much is read from each file at a time. The same figure the rest of the
/// codebase reads with, and the one that bounds the memory: this many bytes
/// times the number of files being compared together.
inline constexpr qint64 kComparisonChunkBytes = 256 * 1024;

/// How many files are ever read in lockstep. Sixteen chunks is four megabytes,
/// and sixteen descriptors is nothing -- while a bucket of ten thousand files
/// held open at once would be two and a half gigabytes and a hard limit hit.
inline constexpr int kMaxOpenAtOnce = 16;

} // namespace mole
