#pragma once

#include "sdk/IThumbnailer.h"

#include <QObject>

#include <memory>
#include <vector>

namespace mole {

/// Everything that can make a picture of a file, highest priority first.
///
/// Unlike MetadataRegistry this stops at the first match: a file has one
/// picture, so the winner answers and the rest are not asked. See
/// docs/adr/0058-a-file-can-say-what-it-looks-like.md.
class ThumbnailRegistry : public QObject, public IThumbnailLookup
{
    Q_OBJECT

public:
    explicit ThumbnailRegistry(QObject* parent = nullptr);
    ~ThumbnailRegistry() override;

    bool addThumbnailer(std::unique_ptr<IThumbnailer> thumbnailer);

    IThumbnailer* thumbnailerFor(const FileEntry& entry) const override;
    IThumbnailer* thumbnailer(const QString& id) const;
    QList<IThumbnailer*> thumbnailers() const;

private:
    /// Kept sorted by descending priority, so which one wins is decided once
    /// here rather than at every lookup.
    std::vector<std::unique_ptr<IThumbnailer>> m_thumbnailers;
};

} // namespace mole
