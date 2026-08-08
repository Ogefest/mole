#pragma once

#include "sdk/IFeature.h"

#include <QAbstractListModel>
#include <QList>

#include <memory>
#include <vector>

namespace mole {

/// The catalogue of available tab types.
///
/// Doubles as the list model behind the new-tab menu, so registering a feature
/// is the only step needed to make it reachable from the UI.
class FeatureRegistry : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        DescriptionRole,
        IconTextRole,
        NeedsContextRole,
    };

    explicit FeatureRegistry(QObject* parent = nullptr);
    ~FeatureRegistry() override;

    /// Ignores a feature whose id is already taken and returns false.
    bool registerFeature(std::unique_ptr<IFeature> feature);

    IFeature* feature(const QString& id) const;
    QList<IFeature*> features() const;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void countChanged();

private:
    std::vector<std::unique_ptr<IFeature>> m_features;
};

} // namespace mole
