#include "host/FeatureRegistry.h"

namespace mole {

FeatureRegistry::FeatureRegistry(QObject* parent)
    : QAbstractListModel(parent)
{
}

FeatureRegistry::~FeatureRegistry() = default;

bool FeatureRegistry::registerFeature(std::unique_ptr<IFeature> feature)
{
    if (!feature || feature->id().isEmpty())
        return false;
    if (this->feature(feature->id()))
        return false;

    const int row = static_cast<int>(m_features.size());
    beginInsertRows({}, row, row);
    m_features.push_back(std::move(feature));
    endInsertRows();
    emit countChanged();
    return true;
}

IFeature* FeatureRegistry::feature(const QString& id) const
{
    for (const auto& feature : m_features) {
        if (feature->id() == id)
            return feature.get();
    }
    return nullptr;
}

QString FeatureRegistry::currentIdFor(const QString& id) const
{
    if (feature(id))
        return id;
    for (const auto& candidate : m_features) {
        if (candidate->absorbedIds().contains(id))
            return candidate->id();
    }
    return id;
}

QList<IFeature*> FeatureRegistry::features() const
{
    QList<IFeature*> out;
    out.reserve(static_cast<qsizetype>(m_features.size()));
    for (const auto& feature : m_features)
        out.append(feature.get());
    return out;
}

int FeatureRegistry::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_features.size());
}

QVariant FeatureRegistry::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};

    const IFeature* feature = m_features.at(static_cast<size_t>(index.row())).get();
    switch (role) {
    case IdRole:
        return feature->id();
    case TitleRole:
    case Qt::DisplayRole:
        return feature->title();
    case DescriptionRole:
        return feature->description();
    case IconTextRole:
        return feature->iconText();
    case NeedsContextRole:
        return feature->needsContext();
    default:
        return {};
    }
}

QHash<int, QByteArray> FeatureRegistry::roleNames() const
{
    return {
        { IdRole, "featureId" },
        { TitleRole, "title" },
        { DescriptionRole, "description" },
        { IconTextRole, "iconText" },
        { NeedsContextRole, "needsContext" },
    };
}

} // namespace mole
