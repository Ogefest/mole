#pragma once

#include "sdk/PluginApi.h"
#include "support/TestSupport.h"

#include <QImage>
#include <QList>
#include <QString>

#include <memory>
#include <vector>

class QQmlApplicationEngine;
class QQuickItem;
class QQuickWindow;
class QTemporaryDir;

namespace mole {
class AppController;
}

namespace mole::test {

/// Runs the whole application headlessly and drives it like a user.
///
/// This replaces the Xvfb-and-xdotool approach, which was not merely awkward
/// but actively misleading: with no window manager there is no X input focus,
/// so keystrokes silently went nowhere. Tests "passed" because nothing
/// happened, and a real bug once looked like a harness problem while a harness
/// problem once looked like a real bug.
///
/// Here `QTest::keyClick` posts to the QQuickWindow directly. No X server, no
/// window manager, no focus lottery -- the same delivery path the application
/// sees in production, minus the display. Screenshots come from
/// `QQuickWindow::grabWindow()`, so a picture is always of the state the
/// assertions just checked.
class QmlAppHarness
{
public:
    struct Options
    {
        /// Where the application starts. Defaults to the harness fixture root.
        QString startUri;
        /// Written to when a test asks for a screenshot. Empty disables them.
        QString screenshotDirectory;
        int windowWidth = 1280;
        int windowHeight = 800;
    };

    QmlAppHarness();
    ~QmlAppHarness();

    /// Creates a private profile, builds the application and loads Main.qml.
    /// Returns false with `errorOut` set rather than asserting, so a test can
    /// report the failure itself.
    bool start(const Options& options, QString* errorOut = nullptr);
    /// Tears the application down and builds it again on the same profile and
    /// the same fixture -- what a restart looks like to whatever was written to
    /// disk, which is the only way to see a credential store that starts shut.
    /// Every pointer from item(), object() or app() is stale afterwards.
    bool restart(QString* errorOut = nullptr);
    void stop();

    AppController* app() const { return m_app.get(); }
    QQuickWindow* window() const { return m_window; }
    /// The fixture directory the application was pointed at.
    QString fixturePath() const;
    QString fixtureUri() const;

    /// Creates a file inside the fixture, making parent folders as needed.
    bool writeFile(const QString& relativePath, const QByteArray& contents = "x");
    bool makeDirs(const QString& relativePath);

    // ---- driving it ------------------------------------------------------

    void key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void type(const QString& text);
    /// Lets bindings, queued task results and the render loop catch up.
    void settle(int rounds = 5);
    /// Spins until `predicate` holds, or the timeout expires.
    bool until(const std::function<bool()>& predicate, int timeoutMs = 10000);

    /// Finds an item by objectName. Walks the visual tree, because
    /// QObject::findChild does not follow what Loader and SplitView build.
    QQuickItem* item(const QString& objectName) const;
    /// Every item with this objectName, in tree order. A dual-pane browser has
    /// two of most things, and one of them is hidden -- asking for "the" item
    /// silently picks whichever comes first.
    QList<QQuickItem*> items(const QString& objectName) const;

    /// Finds any named QML object, not only visual ones. A Dialog is a Popup
    /// rather than an Item, so it never appears in the visual tree that
    /// item() walks.
    QObject* object(const QString& objectName) const;

    /// The item currently holding the keyboard, as a readable type path.
    QString focusChain() const;

    // ---- looking at it ---------------------------------------------------

    /// Renders the window. Empty when screenshots are disabled or grabbing
    /// failed, which the caller should treat as "skip", not as a failure.
    QImage grab();
    /// Renders and writes `<screenshotDirectory>/<name>.png`. Returns the path.
    QString screenshot(const QString& name);

private:
    /// Everything from building the controller to the window being exposed.
    /// Shared by start() and restart(), which differ only in whether the
    /// profile underneath is a new one.
    bool build(QString* errorOut);

    Options m_options;
    std::unique_ptr<PrivateProfile> m_profile;
    std::unique_ptr<QTemporaryDir> m_fixture;
    std::unique_ptr<AppController> m_app;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow* m_window = nullptr;
    QString m_screenshotDirectory;
};

} // namespace mole::test
