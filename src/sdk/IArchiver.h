#pragma once

#include "core/vfs/VfsUri.h"

#include <QList>
#include <QString>

namespace mole {

/// Packing files into one file: something a plugin knows how to do, and the
/// shell only knows how to ask for.
///
/// **The shell used to know a plugin by name.** `AppController` carried seven
/// members behind `#ifdef MOLE_HAVE_ARCHIVE`, and every one of them called a
/// static of the archive plugin's own `CompressTask` -- so `src/ui` knew which
/// plugin writes archives, knew its format table, and was compiled differently
/// depending on whether that plugin's library was available. Every other
/// contribution reaches the shell through this header instead.
///
/// The dialog stays the shell's. What comes from here is what only the plugin
/// can answer -- which kinds exist, what each one is called, whether it carries a
/// password, whether it can hold more than one file -- and the packing itself.
/// A plugin owning the whole dialog was the other reading and needs a route for
/// a dialog the shell does not know, which this fault does not earn. See
/// ADR-0101 and MOLE-415.
class IArchiver
{
public:
    /// One kind of archive, and everything a form has to know to offer it.
    struct Format
    {
        /// What this kind is called. **It is shown as well as stored**: the
        /// picker lists these and hands the chosen one back, so `zip` and
        /// `tar.gz` rather than an internal code nobody can read.
        QString id;
        /// What the file is called: `.zip`, `.tar.gz`. The shell completes a
        /// name with it as the kind is changed, so a `.zip` is never left called
        /// `.tar.gz`.
        QString suffix;
        /// Whether a passphrase means anything here. A box that is offered and
        /// then ignored is worse than one that is not offered: only zip carries
        /// a password, and a tar has no notion of one.
        bool takesPassword = false;
        /// Whether this kind can hold more than one file. A bare `.xz` is one
        /// compressed stream with no container, and discovering that halfway
        /// through writing is too late to tell anybody.
        bool holdsOneFileOnly = false;
    };

    /// What to pack, where, and how.
    struct Request
    {
        /// Files and folders. A folder is packed with what is inside it.
        QList<VfsUri> sources;
        /// The archive to write. The plugin resolves the drives for both ends
        /// itself, through the services it was given.
        VfsUri target;
        /// One of the ids from formats(). Anything else is refused rather than
        /// written under a name that lies about what it is.
        QString formatId;
        /// Empty for an archive anyone can open. Refused for a kind that cannot
        /// carry one -- writing something unencrypted when a password was asked
        /// for is the worst available answer.
        QString passphrase;
        /// Delete the sources once the archive is written, and only after a
        /// complete success.
        bool removeSourcesWhenDone = false;
    };

    virtual ~IArchiver() = default;

    /// What this can write, in the order a picker should offer it. The first is
    /// the default, so put the one anyone can open anywhere first.
    virtual QList<Format> formats() const = 0;

    /// Starts packing, in the background, and answers whether it started.
    ///
    /// **Never blocks**: the caller is the thread that draws the window. The
    /// plugin submits its own task through the services it was given, and posts
    /// what changed on the event bus when it is done -- the shell has no task to
    /// watch and no way to know what was removed.
    virtual bool compress(const Request& request) = 0;
};

} // namespace mole
