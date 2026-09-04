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

    /// This viewer has read the file and cannot show it. `reason` is one phrase
    /// for a person, and the tab steps down the viewer ladder rather than each
    /// viewer deciding for itself what happens next. Emitted by decline().
    void declined(const QString& reason);

protected:
    void setLoading(bool loading);
    void setErrorText(const QString& text);

    /// Gives the file up, after reading it, with a reason a person can read.
    ///
    /// **The second admissibility question, and the only one that can see the
    /// bytes.** canPreview() is a cheap test on the name, the suffix and the
    /// size and must do no I/O -- so everything that decides what showing a file
    /// will actually cost, or whether it can be shown at all, is invisible to
    /// it. By the time the bytes are in hand the viewer is already the one on
    /// screen, and until this existed its only options were an error in an empty
    /// pane or a window that stops answering.
    ///
    /// A decline has a defined outcome rather than being each viewer's private
    /// business: the next viewer down the ladder gets the file, and the strip
    /// says which viewer gave up and why. See ADR-0078.
    ///
    /// Call it once. A viewer that has declined is about to be replaced, and
    /// nothing it says afterwards will be shown.
    void decline(const QString& reason);

private:
    bool m_loading = false;
    QString m_errorText;
};

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

/// Renders one family of file types.
///
/// The second half of "preview as many formats as possible": text, PDF, audio
/// tags, SQLite tables, Parquet schemas -- each is a provider, and each can
/// ship in its own plugin so heavy dependencies stay optional.
///
/// (This comment was stranded: ViewerOption was inserted between it and the
/// class, so the words describing a provider sat above a struct of four
/// QStrings. See MOLE-392.)
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

    /// The next provider below `above` that accepts `entry`, or nullptr when
    /// `above` is already the bottom of the ladder.
    ///
    /// What a decline steps on to. "Below" is the registry's own order -- the
    /// one providerFor() searches -- so the ladder a decline walks down is the
    /// same ladder the first choice was made from, and a plugin that inserts
    /// itself into it is stepped through like anything else.
    virtual IPreviewProvider* providerBelow(const FileEntry& entry, const IPreviewProvider* above) const = 0;
};

} // namespace mole
