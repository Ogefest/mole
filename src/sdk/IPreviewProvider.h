#pragma once

#include "sdk/PluginServices.h"

#include "core/vfs/FileEntry.h"

#include <QObject>
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

    /// Cheap test based on name, suffix and size. Must not do any I/O.
    virtual bool canPreview(const FileEntry& entry) const = 0;

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
