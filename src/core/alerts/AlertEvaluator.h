#pragma once

#include "core/alerts/AlertRule.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

class VfsManager;
class AnalysisStore;

/// Measures what an alert watches and decides whether it has tripped.
///
/// Pure measurement plus a comparison, with no storage of its own: the caller
/// owns the rule and writes the outcome back. That is what makes every metric
/// testable against an in-memory drive rather than against a real disk.
///
/// Every method here touches storage, so it runs on a pool thread only.
class AlertEvaluator
{
public:
    /// What one check found.
    struct Reading
    {
        bool measured = false;
        /// The comparable number. Meaningless when the metric is textual.
        double number = 0.0;
        /// The displayable form, and what Changed compares against.
        QString text;
        QString error;
    };

    /// `analysis` may be null; report-sourced metrics then fail cleanly rather
    /// than silently measuring something else.
    AlertEvaluator(VfsManager* vfs, AnalysisStore* analysis);

    /// Measures the rule's metric. Never throws and never blocks forever --
    /// `cancel` is polled by the tree walk.
    Reading measure(const AlertRule& rule, const CancelToken& cancel) const;

    /// Applies the reading to the rule and returns the updated copy, including
    /// its new state, message and timestamps.
    static AlertRule apply(const AlertRule& rule, const Reading& reading, const QDateTime& at);

private:
    Reading measureLive(const AlertRule& rule, const CancelToken& cancel) const;
    Reading measureFromReport(const AlertRule& rule) const;

    VfsManager* m_vfs = nullptr;
    AnalysisStore* m_analysis = nullptr;
};

} // namespace mole
