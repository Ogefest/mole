#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace mole {

/// Per-tab state, and the object a feature's QML view binds to.
///
/// One instance exists for each open tab; closing the tab destroys it. The
/// base class carries only what the shell needs to render a tab strip -- a
/// label, a subtitle and a busy flag. Everything else a feature needs, it adds
/// as its own Q_PROPERTYs.
class FeatureController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString subtitle READ subtitle NOTIFY subtitleChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)

public:
    explicit FeatureController(QString title, QObject* parent = nullptr);
    ~FeatureController() override;

    QString title() const { return m_title; }
    QString subtitle() const { return m_subtitle; }
    bool isBusy() const { return m_busy; }

    // ---- session state ---------------------------------------------------
    //
    // A tab is restored by re-creating the feature and handing back whatever
    // it saved, so the shell never has to understand what any tab contains.
    // Keys inside the map are entirely yours.

    /// Everything this tab needs in order to come back as it was. Return an
    /// empty map for a tab with nothing worth remembering.
    virtual QVariantMap saveState() const { return {}; }

    /// Called once, right after the controller is created, with whatever
    /// saveState() returned last time. Treat every value as untrusted: it may
    /// come from an older version or a hand-edited file.
    virtual void restoreState(const QVariantMap& state) { Q_UNUSED(state) }

    // ---- where this tab is -----------------------------------------------

    /// The locations this tab currently has open, where that means anything: the
    /// folder each browser pane is showing, the file a preview has open. Empty
    /// for a tab that is not anywhere -- a report, a bulk rename, a schedule.
    ///
    /// The shell asks so that the sidebar can say which drives are in use, which
    /// is what the dot beside a drive means. Asked rather than pushed, and asked
    /// again whenever stateChanged() says something moved, so nothing is polled
    /// and no drive is contacted to find out. It counts whether or not this tab
    /// is the visible one: the question is about the drive, not about the window.
    ///
    /// Answer with uris as strings, in whatever order; the shell resolves each to
    /// a drive and does not care about duplicates. See
    /// docs/adr/0052-a-drives-dot-says-what-it-is-doing.md.
    virtual QStringList openLocations() const { return {}; }

signals:
    void titleChanged();
    void subtitleChanged();
    void busyChanged();
    /// Emit when something worth persisting changed. The shell debounces this
    /// and writes the session file; emitting it often is fine.
    void stateChanged();

protected:
    void setTitle(const QString& title);
    void setSubtitle(const QString& subtitle);
    void setBusy(bool busy);

private:
    QString m_title;
    QString m_subtitle;
    bool m_busy = false;
};

} // namespace mole
