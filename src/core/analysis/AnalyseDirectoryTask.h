#pragma once

#include "core/analysis/AnalysisReport.h"
#include "core/tasks/Task.h"
#include "core/vfs/IFileSystem.h"

namespace mole {

/// Walks a tree once and turns it into an AnalysisReport.
///
/// One pass, aggregating as it goes: a directory worth analysing is usually
/// one too big to hold in memory file by file.
class AnalyseDirectoryTask final : public Task
{
    Q_OBJECT

public:
    AnalyseDirectoryTask(FileSystemPtr fileSystem, VfsUri root, QString label, QObject* parent = nullptr);

    /// Valid once finished() has been delivered.
    const AnalysisReport& report() const { return m_report; }

signals:
    /// Delivered on the UI thread before finished().
    void reportReady(const mole::AnalysisReport& report);

protected:
    void run() override;

private:
    static constexpr int kLargestFilesKept = 25;
    static constexpr int kTopFoldersKept = 30;

    FileSystemPtr m_fileSystem;
    VfsUri m_root;
    QString m_label;
    AnalysisReport m_report;
};

} // namespace mole

Q_DECLARE_METATYPE(mole::AnalysisReport)
