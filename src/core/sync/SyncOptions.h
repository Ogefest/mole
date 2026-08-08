#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace mole {

/// How one tree should be made to resemble another.
///
/// The options are the feature. Everybody's idea of "sync" is different --
/// mirror, top up, never touch what is newer -- and a tool with one behaviour is
/// a tool most people cannot use for the job they have.
struct SyncOptions
{
    enum class Mode {
        /// Copy what is missing or changed. Nothing at the destination is ever
        /// removed. The safe default, and what most people mean.
        Update,
        /// Make the destination match the source exactly, including removing
        /// what the source does not have.
        Mirror,
        /// Copy only what is missing. Existing files are left entirely alone,
        /// however old they are.
        FillGaps
    };

    /// How a file is judged to have changed.
    enum class Compare {
        /// Size or modification time differs. Cheap and right nearly always.
        SizeAndTime,
        /// Size alone. For drives whose timestamps cannot be trusted -- some
        /// network shares and most archive formats.
        SizeOnly,
        /// Contents. Certain, and pays for that by reading both sides.
        Contents
    };

    Mode mode = Mode::Update;
    Compare compare = Compare::SizeAndTime;

    /// Work out what would happen and stop. The plan is built the same way
    /// either way, so this costs nothing to support and is the only honest way
    /// to try a mirror for the first time.
    bool dryRun = true;

    /// Never overwrite a destination file that is newer than its source. Guards
    /// against a stale source undoing work done at the far end.
    bool skipNewer = true;
    /// Descend into subdirectories.
    bool recursive = true;
    /// Copy files whose name begins with a dot, or that the drive marks hidden.
    bool includeHidden = false;

    /// Wildcard patterns. Include wins when both match, because a narrow
    /// include beside a broad exclude is how people express "only these".
    QStringList includePatterns;
    QStringList excludePatterns;

    static QString modeToString(Mode mode);
    static Mode modeFromString(const QString& text);
    static QString modeLabel(Mode mode);
    static QString modeDescription(Mode mode);

    static QString compareToString(Compare compare);
    static Compare compareFromString(const QString& text);
    static QString compareLabel(Compare compare);

    /// Whether a name survives the filters.
    bool accepts(const QString& name, bool hidden) const;

    QJsonObject toJson() const;
    static SyncOptions fromJson(const QJsonObject& json);
};

} // namespace mole
