#pragma once

#include "core/data/JsonFileStore.h"

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>

namespace mole {

/// The small things the application remembers about how someone likes to work.
///
/// One file of dotted keys, and no knowledge of what any of them mean: a viewer
/// that wants to remember something asks for a key and hands over a value. Deliberately
/// not the session file, which is about what was open rather than what was chosen.
/// See docs/adr/0006-preview-options-and-preferences.md.
class Preferences : public JsonFileStore
{
    Q_OBJECT

public:
    /// Honours MOLE_PREFERENCES_PATH, so tests never touch the developer's own.
    static QString defaultPath();

    explicit Preferences(QString path, QObject* parent = nullptr);

    /// False when the file is there and could not be read: it has been kept
    /// beside itself and this store will not write over it. Everything anybody
    /// ever chose is in this one file, and it is replaced whole on every change.
    bool load();
    [[nodiscard]] bool save();

    /// `fallback` when nothing has been remembered, which is the usual case.
    Q_INVOKABLE QVariant value(const QString& key, const QVariant& fallback = {}) const;
    /// Writes and saves. Setting a key to what it already holds does nothing at all,
    /// so a view that assigns on every change does not rewrite the file each time.
    ///
    /// False when the file could not be written. The value is in the model
    /// either way and `changed` is emitted either way -- a view showing
    /// something different from what the application is actually using would be
    /// a third state, worse than the two there are -- and the failure is
    /// reported once through JsonFileStore::saveFailed(). See ADR-0089.
    Q_INVOKABLE bool setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE bool contains(const QString& key) const;
    Q_INVOKABLE bool remove(const QString& key);

signals:
    void changed(const QString& key);

private:
    QVariantMap m_values;
};

} // namespace mole
