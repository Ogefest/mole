#pragma once

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
class Preferences : public QObject
{
    Q_OBJECT

public:
    /// Honours MOLE_PREFERENCES_PATH, so tests never touch the developer's own.
    static QString defaultPath();

    explicit Preferences(QString path, QObject* parent = nullptr);

    bool load();
    bool save() const;

    /// `fallback` when nothing has been remembered, which is the usual case.
    Q_INVOKABLE QVariant value(const QString& key, const QVariant& fallback = {}) const;
    /// Writes and saves. Setting a key to what it already holds does nothing at all,
    /// so a view that assigns on every change does not rewrite the file each time.
    Q_INVOKABLE void setValue(const QString& key, const QVariant& value);
    Q_INVOKABLE bool contains(const QString& key) const;
    Q_INVOKABLE void remove(const QString& key);

signals:
    void changed(const QString& key);

private:
    QString m_path;
    QVariantMap m_values;
};

} // namespace mole
