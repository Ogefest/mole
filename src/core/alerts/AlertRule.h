#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace mole {

/// What an alert watches.
///
/// The set is deliberately small and each entry earns its place by answering a
/// question someone actually asks about storage. Adding one means teaching the
/// evaluator to measure it -- nothing else changes.
enum class AlertMetric {
    TotalSize, ///< bytes in a file, or in a folder tree
    FileCount, ///< files in a tree
    FolderCount, ///< folders in a tree
    FreeSpace, ///< bytes free on the drive holding the target
    FreeSpacePercent, ///< the same as a percentage, which is how people think
    LargestFile, ///< the biggest single file in a tree
    NewestFileAgeHours, ///< hours since anything changed -- a stalled backup
    Permissions, ///< "rwxr-xr--"; a change here is a security event
    ModifiedTime, ///< when the target itself last changed
    Exists, ///< 1 or 0; the plainest alert there is
    UnreadableFolders, ///< folders a scan could not enter
};

/// How the measured value is judged.
enum class AlertComparison {
    Above, ///< value > threshold
    Below, ///< value < threshold
    Changed, ///< value differs from the last one seen
    Equals, ///< value == threshold
};

/// Where the number comes from.
enum class AlertSource {
    /// Measured live by walking or asking the drive. Accurate, and as slow as
    /// the tree is big.
    Live,
    /// Read from the most recent saved analysis report. Instant, and only as
    /// fresh as the last report -- which is the report's job to keep current.
    LatestReport,
};

/// What the alert concluded last time it ran.
enum class AlertState {
    Unknown, ///< never checked
    Ok, ///< measured, within bounds
    Triggered, ///< measured, outside them
    Failed, ///< could not be measured at all
};

QString alertMetricToString(AlertMetric metric);
/// Nothing for a name this build does not know.
///
/// Every one of these refuses rather than picking its first enumerator, and the
/// caller has to say what an unknown name means to it. A stored rule naming one
/// is dropped; a form that has somehow produced one adds nothing. The
/// alternative was in the code and cost more than it saved: an unknown metric
/// loaded as `totalSize`, so a watch saved by a newer build came back as a
/// different alert with the same name and the same target, and an unknown source
/// loaded as `live`, turning a report-backed alert into a full tree walk on every
/// check. `Chain::fromJson` and `SearchPredicate::fromJson` state the same rule
/// for the same reason. See MOLE-163 and MOLE-378.
std::optional<AlertMetric> alertMetricFromString(const QString& text);
/// "Total size", "Free space (%)" -- what the interface shows.
QString alertMetricLabel(AlertMetric metric);
/// The unit a threshold is entered in: "bytes", "files", "hours", "%", "".
QString alertMetricUnit(AlertMetric metric);
/// True when this metric produces a number rather than text.
bool alertMetricIsNumeric(AlertMetric metric);
/// True when this metric can only be answered by a saved report.
bool alertMetricNeedsTree(AlertMetric metric);

QString alertComparisonToString(AlertComparison comparison);
std::optional<AlertComparison> alertComparisonFromString(const QString& text);
QString alertComparisonLabel(AlertComparison comparison);

QString alertSourceToString(AlertSource source);
std::optional<AlertSource> alertSourceFromString(const QString& text);

QString alertStateToString(AlertState state);
/// Unknown for a name this build does not know, which is what Unknown means:
/// never checked. A state is the last check's outcome rather than configuration,
/// so there is nothing here for a stored file to get wrong.
AlertState alertStateFromString(const QString& text);

/// One thing being watched.
struct AlertRule
{
    QString id;
    QString label;
    /// File, folder or drive root. What the metric means depends on it.
    QString targetUri;

    AlertMetric metric = AlertMetric::TotalSize;
    AlertComparison comparison = AlertComparison::Above;
    AlertSource source = AlertSource::Live;
    /// Compared against for Above/Below/Equals. Ignored by Changed.
    double threshold = 0.0;
    bool enabled = true;

    // ---- what the last check found --------------------------------------

    AlertState state = AlertState::Unknown;
    /// The measured value as text, which is the only form that works for both
    /// a byte count and a permission string.
    QString lastValue;
    double lastNumericValue = 0.0;
    QDateTime lastCheckedAt;
    /// When it most recently went from Ok to Triggered.
    QDateTime triggeredAt;
    QString message;

    bool isValid() const { return !id.isEmpty() && !targetUri.isEmpty(); }
    /// Human sentence: "Total size above 10 GB".
    QString describe() const;

    QJsonObject toJson() const;
    /// Nothing when the stored rule names a metric, comparison or source this
    /// build does not know. The caller drops it and says how many it dropped.
    [[nodiscard]] static std::optional<AlertRule> fromJson(const QJsonObject& json);
};

/// One check, kept so a spike last night is still visible this morning.
struct AlertEvent
{
    QString ruleId;
    QString ruleLabel;
    QDateTime at;
    AlertState state = AlertState::Unknown;
    QString value;
    QString message;

    QJsonObject toJson() const;
    static AlertEvent fromJson(const QJsonObject& json);
};

} // namespace mole
