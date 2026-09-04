#pragma once

#include "core/data/JsonFileStore.h"

#include <QList>
#include <QString>
#include <QVariantMap>

namespace mole {

/// One remembered tab: which feature to re-create, and the state it saved.
struct TabSession
{
    QString featureId;
    QVariantMap state;
};

/// How the window was showing.
///
/// Three states rather than "maximised or not", because only `Normal` carries a
/// size worth keeping: a maximised window and a full-screen one both report the
/// screen's metrics, and writing either of those over the remembered size is
/// how a window somebody had sized by hand comes back the size of the display.
enum class WindowState { Normal, Maximized, FullScreen };

/// The name a state is written under, in the session file and on the way to
/// QML. One spelling for both, so what is written and what is read cannot drift
/// apart.
QString windowStateName(WindowState state);
/// Unknown names read as Normal: a session file is allowed to be from a later
/// version, or hand-edited, and starting in an ordinary window is the answer
/// that is never wrong.
WindowState windowStateFromName(const QString& name);

/// Where the window was and how big. Restored only when it still lands on a
/// screen that exists -- monitors get unplugged between sessions.
struct WindowGeometry
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    /// **Whether x and y mean anything, said rather than encoded.**
    ///
    /// "Unset" was `x == -1`, and -1 is a coordinate: a monitor to the left of
    /// or above the primary one has negative virtual-desktop coordinates, so a
    /// window left there was read as having no position and came back on the
    /// primary screen every time. `geometryIsOnScreen()` already asks the real
    /// question -- does this rectangle still land on a screen -- and a sentinel
    /// in front of it answered a different one. See MOLE-395.
    bool positionKnown = false;
    WindowState state = WindowState::Normal;

    bool isValid() const { return width > 0 && height > 0; }
    bool hasPosition() const { return positionKnown; }
};

struct Session
{
    QList<TabSession> tabs;
    int currentIndex = -1;
    WindowGeometry window;

    bool isEmpty() const { return tabs.isEmpty() && !window.isValid(); }
};

/// Reads and writes the open-tabs file.
///
/// The format is plain JSON on purpose: it is small, a person can read it when
/// something goes wrong, and a hand-edited or half-written file degrades into
/// "start fresh" rather than a failure to launch.
class SessionStore
{
public:
    explicit SessionStore(QString filePath);

    /// `MOLE_SESSION_PATH` wins, so tests and throwaway runs never touch the
    /// user's real session.
    static QString defaultFilePath();

    QString filePath() const { return m_file.path(); }

    /// Writes atomically -- a crash mid-write must not leave a truncated file
    /// that loses every tab.
    ///
    /// False when nothing landed, with the reason in `reasonOut` for whoever
    /// can say it out loud. Not a signal, because this is not a QObject and the
    /// one caller is the shell. It used to be called as a statement on the way
    /// out of the application, so a session that could not be written was a
    /// window that came back wrong with nothing said at any point. See ADR-0089.
    [[nodiscard]] bool save(const Session& session, QString* reasonOut = nullptr);

    /// Returns an empty session when the file is missing, unreadable or not
    /// what we expect. Never throws and never reports an error: a broken
    /// session is not worth refusing to start over.
    Session load() const;

    void clear() const;

private:
    /// The same writer every other store uses: atomic, and one warning when it
    /// could not. The *reading* here stays as it was -- a session file that
    /// cannot be parsed degrades to "start fresh", which ARCHITECTURE.md decided
    /// and which is right for a file the user rebuilds by using the application.
    /// A drive list or a schedule is not that, which is why they keep theirs.
    JsonFile m_file;
};

} // namespace mole
