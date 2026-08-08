#pragma once

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

/// Where the window was and how big. Restored only when it still lands on a
/// screen that exists -- monitors get unplugged between sessions.
struct WindowGeometry
{
    int x = -1;
    int y = -1;
    int width = 0;
    int height = 0;
    bool maximized = false;

    bool isValid() const { return width > 0 && height > 0; }
    bool hasPosition() const { return x > -1 && y > -1; }
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

    const QString& filePath() const { return m_filePath; }

    /// Writes atomically -- a crash mid-write must not leave a truncated file
    /// that loses every tab.
    bool save(const Session& session) const;

    /// Returns an empty session when the file is missing, unreadable or not
    /// what we expect. Never throws and never reports an error: a broken
    /// session is not worth refusing to start over.
    Session load() const;

    void clear() const;

private:
    QString m_filePath;
};

} // namespace mole
