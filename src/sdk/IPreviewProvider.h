#pragma once

#include "sdk/PluginServices.h"

#include "core/vfs/FileEntry.h"

#include <QObject>
#include <QStringList>
#include <QUrl>

namespace mole {

/// Backing object for one open preview. Loads its content off the UI thread
/// through the task manager and exposes it to QML however it likes.
class PreviewController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadingChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

public:
    explicit PreviewController(QObject* parent = nullptr);
    ~PreviewController() override;

    bool isLoading() const { return m_loading; }
    QString errorText() const { return m_errorText; }

    /// Called once after construction with the entry to display.
    virtual void load(const FileEntry& entry) = 0;

    /// One of the choices the provider declared, applied before load(). Ignored by
    /// default, so a viewer with no options needs no code for this at all.
    virtual void setViewerOption(const QString& key, const QString& value)
    {
        Q_UNUSED(key);
        Q_UNUSED(value);
    }

signals:
    void loadingChanged();
    void errorTextChanged();

protected:
    void setLoading(bool loading);
    void setErrorText(const QString& text);

private:
    bool m_loading = false;
    QString m_errorText;
};

/// Renders one family of file types.
///
/// The second half of "preview as many formats as possible": text, PDF, audio
/// tags, SQLite tables, Parquet schemas -- each is a provider, and each can
/// ship in its own plugin so heavy dependencies stay optional.
/// How a viewer behaves, offered to whoever is looking at the file.
///
/// The provider says what the choices are; the strip above the preview renders them
/// without knowing what any of them mean, and the answer is remembered per file type.
/// See docs/adr/0006-preview-options-and-preferences.md.
struct ViewerOption
{
    /// Provider-local, e.g. "mode". Becomes part of the preference key.
    QString key;
    /// Shown to the reader, e.g. "Show".
    QString title;
    QStringList choices;
    QString defaultChoice;
};

class IPreviewProvider
{
public:
    virtual ~IPreviewProvider() = default;

    /// Stable, namespaced identifier.
    virtual QString id() const = 0;
    virtual QString displayName() const = 0;

    /// When several providers accept the same file the highest priority wins,
    /// so a dedicated SQLite viewer beats the generic hex dump.
    virtual int priority() const { return 0; }

    /// What can be chosen about how this file is shown. Empty for most viewers,
    /// and it takes the entry because the answer depends on it: an .html has a
    /// source-or-rendered choice and a .log has nothing to choose.
    virtual QList<ViewerOption> options(const FileEntry& entry) const
    {
        Q_UNUSED(entry);
        return {};
    }

    /// Cheap test based on name, suffix and size. Must not do any I/O.
    virtual bool canPreview(const FileEntry& entry) const = 0;

    /// Whether the details panel starts open for this viewer, the first time
    /// somebody looks at a file of a type. True for a viewer whose content the
    /// details are -- the information viewer shows nothing else -- and false for
    /// one that has a file to show. Whatever the reader then chooses is
    /// remembered per file type and outranks this.
    /// See docs/adr/0034-what-a-file-says-about-itself.md.
    virtual bool detailsOpenByDefault() const { return false; }

    /// QML component rendering the preview, given a `controller` property.
    virtual QUrl viewSource() const = 0;

    virtual PreviewController* createController(QObject* parent) = 0;
};

/// Finds the viewer for a file. Implemented by the host; handed to plugins
/// through PluginServices so a preview tab can ask without reaching into the
/// shell.
class IPreviewLookup
{
public:
    virtual ~IPreviewLookup() = default;

    /// Best provider for `entry`, or nullptr when nothing can render it.
    virtual IPreviewProvider* providerFor(const FileEntry& entry) const = 0;
};

} // namespace mole
