#pragma once

#include "sdk/PluginServices.h"

#include "core/index/IndexDatabase.h"
#include "core/index/ScanOptions.h"
#include "core/vfs/IFileSystem.h"

#include <functional>

namespace mole {

class ScanTask;
class Task;

/// The two readers that turn a bare walk of a tree into a complete scan, and
/// the one call that installs them.
///
/// They live here rather than beside the search form because more than one
/// caller has to be able to build the same scan: the form, the nightly
/// re-index, and the browser's own "index this folder". While they were private
/// to the form, the nightly re-index rewrote every subtree it re-walked with
/// rows carrying no metadata and no archive members -- the feature got worse the
/// longer it ran. `sdk` is the lowest layer all three can reach.
/// See docs/adr/0056-a-scan-is-asked-for-in-one-place.md.

/// What a file says about itself, through the readers that fill the details
/// panel. The same readers, so the index and the panel can never disagree.
///
/// **The readers get the services and the scan's own cancel token.** They used
/// to get `PluginServices {}` and `CancelToken {}`, and every shipped reader
/// that needs bytes past the sniff page checks `services.vfs` and gives up
/// without one -- so a JPEG whose EXIF sits past the first page, a docx whose
/// core.xml is not at the front and an audio file whose tags are at the end were
/// all described in the panel and indexed as nothing. "Every photo taken with
/// camera X" missed the files the drawer was naming. See MOLE-382.
///
/// Null when nothing is registered to read a file, which is what a headless
/// context looks like from here.
std::function<QList<SearchFact>(const FileEntry&, const CancelToken&)> factReaderFor(
    const PluginServices& services, const FileSystemPtr& fileSystem);

/// Rows for what lives inside a container, through whichever mounted backend
/// claims that kind of file. Null when nothing can open one, which is what a
/// build without the archive plugin looks like from here.
///
/// `root` decides how careful the reader is: on a remote drive, opening a
/// container means fetching it whole, so a large one is left alone.
std::function<QList<IndexedFile>(const FileEntry&, bool*)> containerReaderFor(
    const PluginServices& services, const VfsUri& root);

/// Asks `task` for exactly what `options` says, readers and all.
///
/// One line instead of three, so a caller cannot set two of them and forget the
/// third -- which is the fault this exists to stop coming back.
void applyScanOptions(ScanTask& task, const ScanOptions& options, const PluginServices& services,
    const FileSystemPtr& fileSystem, const VfsUri& root);

/// The scan already running over `root`, or nothing.
///
/// **One scan per volume at a time.** Two at once have the second one's
/// generation swap drop the first one's rows, so the second is not a slower
/// answer -- it is a wrong one. The indexes tab asked this before starting a
/// rescan and the other two callers did not, so a nightly rule firing while a
/// manual scan was running started a second walk of the same volume. Asked here
/// because all three can reach here, which is the same reason applyScanOptions()
/// is here. See MOLE-340.
Task* scanRunningOn(const PluginServices& services, const VfsUri& root);

} // namespace mole
