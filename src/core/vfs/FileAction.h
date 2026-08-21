#pragma once

#include "core/vfs/VfsUri.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <utility>

namespace mole {

/// Something one drive can do that another cannot.
///
/// VfsCapability answers a different question. Every member of it is an
/// operation the *core* has to understand by name -- a copy branches on Write, a
/// preview on RandomAccessRead, the sweep on ReportsLeftovers -- so each one is
/// an `if` somewhere above the backend, and that is right for what it holds.
///
/// This tier is for what only the *user* acts on: a filesystem that keeps
/// earlier states of a file, a container that will hand out a link to an object
/// that expires. Nothing above the backend decides anything from one, so nothing
/// above the backend has to know it exists -- which is what lets a drive written
/// next year offer something nobody has thought of yet. It is the shape
/// MenuAction already uses, for the reason its own header gives: "the shell
/// places it without knowing what it does."
///
/// See docs/adr/0075-a-drive-offers-what-only-it-can-do.md.
struct FileAction
{
    /// Stable and namespaced, e.g. "org.example.objects.link". Stable because it
    /// is what comes back to invoke(), and what anything remembering a choice --
    /// a shortcut, a step in a chain -- would have to write down.
    QString id;
    /// Shown as it is written, in the drive's own words: "Earlier versions".
    QString title;
    /// False greys the entry out rather than hiding it, so a list of what a drive
    /// can do does not change shape as you move down a folder.
    bool enabled = true;
};

using FileActionList = QList<FileAction>;

/// What invoking one gave back. Two kinds, and the set is closed.
///
/// Closed is the whole point: the interface can present the answer to an action
/// it has never heard of, because there are only two things an answer can be.
/// Text is shown with a way to copy it; a list of uris is offered as a list to
/// open from. A third kind would be a third branch above every backend and a
/// decision to take deliberately, not an addition to make in passing.
///
/// "It returns a task" is not a third kind. IFileSystem is synchronous by
/// contract and every call into it already runs on a TaskManager worker, so an
/// action is always invoked as a task and only the answer varies.
struct FileActionOutcome
{
    enum class Kind {
        /// Something to read and copy: a link, a checksum, a container's policy.
        Text,
        /// Other uris for the same file -- earlier versions of it, mostly. Each
        /// one opens as an ordinary file, because that is what it is.
        Uris,
    };

    Kind kind = Kind::Text;
    /// Set when kind is Text.
    QString text;
    /// Set when kind is Uris.
    QList<VfsUri> uris;
    /// When `text` stops being true, for the answers that go stale: a link
    /// signed for ten minutes is worth showing with the ten minutes. Invalid
    /// when the answer does not expire, which is most of them.
    QDateTime validUntil;

    static FileActionOutcome fromText(QString value, QDateTime until = {})
    {
        FileActionOutcome outcome;
        outcome.kind = Kind::Text;
        outcome.text = std::move(value);
        outcome.validUntil = std::move(until);
        return outcome;
    }

    static FileActionOutcome fromUris(QList<VfsUri> value)
    {
        FileActionOutcome outcome;
        outcome.kind = Kind::Uris;
        outcome.uris = std::move(value);
        return outcome;
    }

    /// Whether it carries what its kind promises. An outcome that does not is
    /// neither kind whatever it says it is, and there is nothing to show for it.
    bool isValid() const { return kind == Kind::Text ? !text.isEmpty() : !uris.isEmpty(); }
};

/// What a drive turned out to be able to offer where it was pointed, and whether
/// it has been asked yet.
///
/// The class cannot answer this. The same local backend has earlier states of a
/// file on one filesystem and not on another; the same object-store backend has
/// them on one container and not on another. One class, two answers -- so the
/// answer is discovered from the drive rather than compiled in, once, at the
/// moment somebody first opens a folder on it. See ADR-0076.
///
/// **Three states and not two, because absent and not-yet-known are different
/// and only one of them is a lie.** Before the answer arrives the drive has not
/// said it cannot do the thing; it has said nothing. Anything drawing "no" out of
/// that silence is telling the user something no drive ever said, and would then
/// have to un-say it a moment later.
struct DriveOffers
{
    enum class State {
        /// Nobody has needed the answer yet, so nobody has asked. The state
        /// every drive is in until a folder on it is opened.
        Unasked,
        /// The drive was asked and this is what it said, including when what it
        /// said was nothing.
        Answered,
        /// The drive was asked and could not say -- the far end refused, the
        /// call timed out. Absent, like an empty answer, but for a reason worth
        /// telling apart from one.
        Failed,
    };

    State state = State::Unasked;
    /// The ids of the actions this drive can offer here at all, in the same
    /// namespace FileAction::id uses. What actionsFor() then narrows to one node.
    QStringList ids;

    /// Whether the drive has answered. False while nothing has been asked *and*
    /// when the asking failed: both mean there is nothing to show.
    bool isKnown() const { return state == State::Answered; }
    bool has(const QString& id) const { return state == State::Answered && ids.contains(id); }
};

} // namespace mole
