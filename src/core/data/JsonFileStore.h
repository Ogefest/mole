#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace mole {

/// The plumbing every hand-rolled JSON store in Mole had written out again.
///
/// Ten of them read a file the same way -- open, parse, check a version -- and
/// wrote it the same way, through `QSaveFile` into a directory they had to
/// create first. Thirteen worked out their own path from the same
/// `MOLE_…_PATH` environment override. The repetition was not the cost; the
/// cost was that the same two faults were in all of them at once.
///
/// **A write that did not land said nothing.** `save()` returned `bool` and
/// almost every caller used it as a statement, so a full disk, a read-only
/// configuration directory or a `mkpath` that failed left the model changed, the
/// interface showing the change, and the file as it was. Bookmarks, alert rules,
/// schedules, sets, preferences and the session were then lost at the next
/// start, with nothing said at any point. A schedule is the worst of them:
/// ARCHITECTURE.md's "a job that quietly never runs is the one failure nobody can
/// diagnose" is about exactly this file.
///
/// **A file that could not be parsed was replaced by an empty one.** Four
/// stores cleared their list, failed to parse, returned false and kept nothing
/// -- and the next write put the empty list over the file. One stray byte in
/// `drives.json` and every configured drive was gone, its secrets orphaned in
/// the credential store under ids nobody held any more.
///
/// So: `writeRoot()` is `[[nodiscard]]` and reports its own failure once, and a
/// file that cannot be parsed is moved aside rather than overwritten. See
/// ADR-0089.
/// The file itself: read it, write it atomically, keep it when it cannot be read.
///
/// No QObject and no signals, so a model that already has a base class -- the
/// bookmarks are a QAbstractListModel -- can hold one of these and get exactly
/// the same behaviour as a store that derives from JsonFileStore below.
class JsonFile
{
public:
    explicit JsonFile(QString path);

    /// Where a store keeps its file: the environment override when one is set,
    /// and `fileName` under the application's data directory otherwise.
    ///
    /// The override is what every test tier points at a temporary directory, so
    /// a suite never writes into the account running it.
    static QString pathFor(const char* environmentVariable, const QString& fileName);

    /// The file this store reads and writes.
    QString path() const { return m_path; }

    /// What a load found.
    enum class Read {
        Missing, ///< nothing there yet, which is what a first run looks like
        Loaded, ///< the caller's object or array holds it
        Damaged, ///< there and unreadable; it has been moved aside
    };

    /// Reads the file into `rootOut`.
    ///
    /// Missing is not a failure and never was -- a store with no file yet is a
    /// feature nobody has used. Damaged is the one that used to read the same
    /// as Missing, and it is the difference between "there is nothing" and
    /// "there is something and I cannot read it".
    Read readRoot(QJsonObject* rootOut);
    /// The same, for a file whose top level is an array.
    Read readArray(QJsonArray* arrayOut);

    /// Writes `document` to the file, atomically, creating the directory if it
    /// is not there. False when nothing landed; `reasonOut` says why, in words
    /// with no path in them.
    [[nodiscard]] bool write(const QJsonDocument& document, QString* reasonOut);

    bool isDamaged() const { return m_damaged; }
    QString damagedCopyPath() const { return m_damagedCopy; }
    void discardDamage();

private:
    Read readDocument(QJsonDocument* documentOut);
    /// Moves the unreadable file to `<name>.broken-<timestamp>` and remembers
    /// where. False when even that could not be done, which still counts as
    /// damage: the point is not to write over it.
    bool keepWhatCannotBeRead();

    QString m_path;
    bool m_damaged = false;
    QString m_damagedCopy;
};

class JsonFileStore : public QObject
{
    Q_OBJECT

public:
    /// Where a store keeps its file. See JsonFile::pathFor().
    static QString pathFor(const char* environmentVariable, const QString& fileName)
    {
        return JsonFile::pathFor(environmentVariable, fileName);
    }

    /// The file this store reads and writes.
    QString path() const { return m_file.path(); }

    /// Whether the last load found a file it could not read.
    ///
    /// What was there is kept beside it, at damagedCopyPath(), and the store
    /// then goes on working -- once the old file is safe there is nothing left
    /// to lose by writing a new one, and refusing for the rest of the session
    /// would leave the feature stuck with only a message to explain it.
    ///
    /// **Writing is refused only while the unreadable file is still there**,
    /// which happens when it could not be moved at all -- a read-only directory.
    /// That refusal lifts on a load that succeeds, or on discardDamage().
    bool isDamaged() const { return m_file.isDamaged(); }
    /// Where the unreadable file was moved, or empty. Named so a caller can put
    /// it in front of somebody who has to decide what to do about it.
    QString damagedCopyPath() const { return m_file.damagedCopyPath(); }
    /// Accepts the loss and allows writing again. "Start from what is on
    /// screen", which is a decision only the person looking at it can take.
    void discardDamage() { m_file.discardDamage(); }

signals:
    /// Something could not be written, with the reason in words.
    ///
    /// **No path in it.** The shell turns this into a notification and the
    /// session log records it, and neither may name a directory on somebody's
    /// machine. See ADR-0064.
    void saveFailed(const QString& reason);
    /// A file that could not be parsed was moved aside rather than overwritten,
    /// and this is where it went.
    void loadFoundDamage(const QString& movedTo);

protected:
    explicit JsonFileStore(QString path, QObject* parent = nullptr);

    using Read = JsonFile::Read;

    /// Reads the file into `rootOut`, announcing damage if it finds any.
    Read readRoot(QJsonObject* rootOut);

    /// Writes `root` to the file, atomically.
    ///
    /// False means nothing landed, and it has already been said: one warning
    /// and one `saveFailed`, here rather than at each of the callers that used
    /// to say nothing at all.
    [[nodiscard]] bool writeRoot(const QJsonObject& root);

private:
    JsonFile m_file;
};

} // namespace mole
