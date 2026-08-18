#include "core/CoreMetaTypes.h"

#include "core/duplicates/FindDuplicatesTask.h"
#include "core/search/SearchQuery.h"
#include "core/sync/SyncTask.h"
#include "core/vcs/Repository.h"
#include "core/vfs/FileEntry.h"
#include "core/vfs/VfsTypes.h"
#include "core/vfs/VfsUri.h"

#include <QMetaType>

namespace mole {

void registerCoreMetaTypes()
{
    qRegisterMetaType<VfsUri>("mole::VfsUri");
    qRegisterMetaType<FileEntry>("mole::FileEntry");
    qRegisterMetaType<FileEntryList>("mole::FileEntryList");
    // Crosses a thread boundary: a content search finds its reason on a pool
    // thread and the row that shows it is drawn on the other one.
    qRegisterMetaType<ContentMatch>("mole::ContentMatch");
    qRegisterMetaType<QList<ContentMatch>>("QList<mole::ContentMatch>");
    // Crosses a thread boundary: an operation fails on whichever thread was
    // running it, and what failed decides who should care.
    qRegisterMetaType<VfsError>("mole::VfsError");
    // Crosses a thread boundary: the space query answers from a pool thread.
    qRegisterMetaType<SpaceInfo>("mole::SpaceInfo");
    qRegisterMetaType<AccessInfo>("mole::AccessInfo");
    qRegisterMetaType<QList<DuplicateGroup>>("QList<mole::DuplicateGroup>");
    qRegisterMetaType<SyncPlan>("mole::SyncPlan");
    // Crosses a thread boundary: git state is read on a pool thread and shown by
    // the band above a listing.
    qRegisterMetaType<RepositoryHead>("mole::RepositoryHead");
    qRegisterMetaType<RepositoryStatus>("mole::RepositoryStatus");
}

} // namespace mole
