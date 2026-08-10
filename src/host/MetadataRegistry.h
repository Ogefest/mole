#pragma once

#include "sdk/IMetadataReader.h"

#include <QObject>

#include <memory>
#include <vector>

namespace mole {

/// Every reader that can say something about a file, in the order their facts
/// are shown.
///
/// Unlike PreviewRegistry this does not stop at the first match: a file is shown
/// one way but described from as many angles as there are readers for it. See
/// docs/adr/0034-what-a-file-says-about-itself.md.
class MetadataRegistry : public QObject, public IMetadataLookup
{
    Q_OBJECT

public:
    explicit MetadataRegistry(QObject* parent = nullptr);
    ~MetadataRegistry() override;

    bool addReader(std::unique_ptr<IMetadataReader> reader);

    QList<IMetadataReader*> readersFor(const FileEntry& entry) const override;
    IMetadataReader* reader(const QString& id) const;
    QList<IMetadataReader*> readers() const;

private:
    /// Kept sorted by descending priority, so the order facts are shown in is
    /// decided once here rather than at every lookup.
    std::vector<std::unique_ptr<IMetadataReader>> m_readers;
};

} // namespace mole
