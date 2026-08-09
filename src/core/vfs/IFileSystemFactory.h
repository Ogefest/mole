#pragma once

#include "core/vfs/IFileSystem.h"

#include <QString>
#include <QVariantMap>

namespace mole {

/// Describes one field a backend needs in order to connect. The mount dialog
/// builds its form from this list, so adding an SFTP port option is a data
/// change, not a UI change.
struct ConnectionField
{
    enum Kind {
        Text,
        /// Never written to the settings file. The host puts it in the
        /// encrypted credential store and hands it back at connect time, so a
        /// backend never has to think about where its password lives.
        Password,
        Number,
        Boolean,
        Directory,
        /// One of `choices`, with `choiceLabels` for what to call them.
        Choice
    };

    QString key;
    QString label;
    Kind kind = Text;
    QVariant defaultValue;
    bool required = true;

    /// One line on what it is for. Backends have fields nobody can guess the
    /// meaning of, and a form without help is a form filled in by trial.
    QString help;
    /// Hidden behind "show advanced". A backend with eighty options is
    /// unusable without this, and several have eighty options.
    bool advanced = false;
    QStringList choices;
    QStringList choiceLabels;
    /// Only shown when `dependsOnKey` currently holds one of `dependsOnValues`.
    /// S3 asks completely different questions for AWS and for Ceph.
    QString dependsOnKey;
    QStringList dependsOnValues;
};

/// One kind of drive a factory can make.
///
/// A factory usually offers exactly one, and every backend written by hand does.
/// The mechanism exists for a factory that wraps something with many providers of
/// its own: the host must not have to know which is which, so the list is data.
struct BackendVariant
{
    QString id;
    QString label;
    QString description;
    QList<ConnectionField> fields;
};

/// Creates IFileSystem instances of one scheme. Register these with
/// VfsManager at startup; later they can just as well come from a plugin.
class IFileSystemFactory
{
public:
    virtual ~IFileSystemFactory() = default;

    virtual QString scheme() const = 0;
    /// Shown in the "add drive" menu, e.g. "SSH / SFTP".
    virtual QString displayName() const = 0;
    virtual QString iconName() const { return QStringLiteral("drive"); }

    /// Empty for backends that need no configuration (local disk).
    virtual QList<ConnectionField> connectionFields() const { return {}; }

    /// The kinds of drive this factory can make. Empty means "just one", using
    /// connectionFields() -- which is every backend written by hand. A factory
    /// wrapping something with many providers returns one entry each, and the
    /// chosen id arrives in the config under `variantKey()`.
    virtual QList<BackendVariant> variants() const { return {}; }
    static QString variantKey() { return QStringLiteral("__variant"); }

    /// Whether this factory can actually be used right now. A backend whose
    /// library is missing says so here rather than failing at connect time,
    /// so the interface can leave it out of the list instead of offering
    /// something that cannot work.
    virtual bool isAvailable() const { return true; }
    /// Why not, when it is not.
    virtual QString unavailableReason() const { return {}; }

    /// Lowercased suffixes this backend can mount straight from a file, so
    /// activating one in the browser turns it into a drive. Archives use it
    /// today; an .iso or .sqlite backend would use it the same way, with no
    /// change anywhere in the host.
    virtual QStringList mountableFileSuffixes() const { return {}; }

    /// Config for mounting `localPath`, when its suffix is one of the above.
    virtual QVariantMap configForFile(const QString& localPath) const
    {
        Q_UNUSED(localPath)
        return {};
    }

    /// Root uri of the mount that configForFile() would produce.
    virtual VfsUri rootUriForFile(const QString& localPath) const
    {
        Q_UNUSED(localPath)
        return {};
    }

    /// Builds a ready-to-use backend from the values collected for
    /// connectionFields(). Returning a null pointer means "bad config".
    virtual FileSystemPtr create(const QVariantMap& config, QString* errorOut) = 0;
};

} // namespace mole
