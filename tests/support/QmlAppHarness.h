#pragma once

#include "sdk/PluginApi.h"
#include "support/TestSupport.h"

#include <QDateTime>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QString>

#include <memory>
#include <vector>

class QMimeData;
class QQmlApplicationEngine;
class QQuickItem;
class QQuickWindow;
class QTemporaryDir;

namespace mole {
class AppController;
class ThumbnailImageProvider;
class ThumbnailPump;
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
        ///
        /// Setting it also fixes the fixture directory's name. The random name a
        /// temporary directory gets is right for an ordinary run -- several can go
        /// in parallel -- and wrong for a screenshot run, because the name is in
        /// the tab and in the breadcrumbs of every picture: `mole-tests-wAYrZa`
        /// both looks like nothing anybody has on their disk and makes two
        /// regenerations of the same commit differ in thirty-four files.
        QString screenshotDirectory;
        /// One rung up the same 16:10 ladder the guide's pictures were taken on
        /// at 1280x800: a quarter more pixels, and a picture that still opens
        /// whole on an ordinary laptop screen. 1600x1000 was considered and needs
        /// a viewport wider than 1600 to be looked at without scrolling, which is
        /// not what most people read the guide on. Not a device pixel ratio of 2
        /// either: sharper on a high-density screen, four times the weight, and
        /// GitHub scales the image to the page width anyway.
        int windowWidth = 1440;
        int windowHeight = 900;
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
    /// Creates a file that reports `bytes` and occupies almost none.
    ///
    /// `QFile::resize()` makes a sparse file, so a 1.2 GB video costs no disk and
    /// still says how big it is -- which is what lets the guide's pictures show
    /// the range of sizes Mole exists for without committing gigabytes to a test
    /// fixture. Not for anything that reads the contents: they are zeroes.
    bool writeSparseFile(const QString& relativePath, qint64 bytes);
    /// Sets a file's modification time, so the date column holds more than one
    /// date and a listing sorted by age has something to sort.
    bool setModified(const QString& relativePath, const QDateTime& when);
    /// Gives everything under `relativePath` that still carries the clock a date
    /// derived from its own path, at or before `epoch`.
    ///
    /// The complement of setModified(), which is a hand-written list -- and a
    /// hand-written list of what must not carry the clock goes stale the first
    /// time somebody adds a fixture file. It had: three photographed views showed
    /// files nobody had listed, so their date column read as today and the
    /// pictures were rewritten every day. Anything already stamped at or before
    /// `epoch` is left exactly as it is, so the explicit dates still win.
    ///
    /// `.git` is skipped. Rewriting the times inside a checkout makes libgit2
    /// recheck what it had cached and can change the very answer the picture is of.
    ///
    /// Deterministic from the path rather than random, and a hash of our own
    /// rather than qHash, whose seed is not promised to be stable between Qt
    /// versions -- an upgrade must not rewrite the guide. See MOLE-255.
    bool fixDatesUnder(const QString& relativePath, const QDate& epoch);
    bool makeDirs(const QString& relativePath);

    // ---- driving it ------------------------------------------------------

    void key(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void type(const QString& text);

    // ---- the pointer -----------------------------------------------------
    //
    // Delivered to the QQuickWindow through QTest, the same path `key()` uses and
    // for the same reason: no X server, no window manager, no focus lottery.
    // Positions are in window coordinates -- `centreOf()` turns an item into one.

    void press(const QPoint& where, Qt::MouseButton button = Qt::LeftButton);
    void moveTo(const QPoint& where);
    void release(const QPoint& where, Qt::MouseButton button = Qt::LeftButton);
    void click(const QPoint& where);
    /// Clicks an item once the layout has actually put it somewhere.
    ///
    /// `click(centreOf(item))` reads a position and sends a click at it, and those
    /// are two different instants. An item that has just become visible reports the
    /// position it has *before* the layout pass places it -- which for a child of a
    /// ColumnLayout is the top of its parent, on top of whatever is really there.
    /// Under load that is what happened: the repository band's changed-count label
    /// reported scene (260,154), the pane's back button occupies (252,154) 48x48,
    /// and the click at the label's centre went to the back button. The pane
    /// navigated away, the band cleared, and the test failed saying the count did
    /// not open. Once in about fifty runs, and never in isolation. See MOLE-256.
    ///
    /// So the position is read until two consecutive reads agree, with the event
    /// loop turned between them -- a condition, not a wait long enough to be
    /// probably fine. A layout pass runs on polish, so one round of the loop is
    /// enough for the position to change; two the same means it has settled.
    /// Returns false if it never settles, which is a real answer rather than a
    /// click sent somewhere arbitrary.
    bool clickOn(QQuickItem* item);
    void doubleClick(const QPoint& where);
    /// Presses at `from` and moves well past the platform's drag threshold, which
    /// is what turns a press into a drag. The button is still down when this
    /// returns: a drag in flight is the state most of these tests are about.
    void dragFrom(const QPoint& from, const QPoint& to = {});
    /// Where an item is, in window coordinates. Empty items and items outside
    /// the window are still answered for -- what a test does with that is its
    /// own business.
    QPoint centreOf(const QQuickItem* item) const;

    // ---- a drag from another application ---------------------------------
    //
    // A real drag cannot be driven from a test: QDrag::exec() wants a platform, a
    // pointer and a nested event loop, and this binary has none of them. What can
    // be driven is everything downstream of the events a real drag produces --
    // which is all of Mole's own behaviour -- so the payload is built here and
    // delivered to the window as QDragEnterEvent, QDragMoveEvent and QDropEvent.
    //
    // The offered actions are copy *and* move, the way a file manager sending a
    // drag proposes both. A receiver that took the proposed action would be
    // telling the sender it may delete; what Mole does instead is asserted rather
    // than assumed.

    /// Starts a drag of `localPaths` over the window at `where`. The payload is
    /// kept until dropAt() or dragLeave(), the way one real drag holds one.
    void dragEnter(const QStringList& localPaths, const QPoint& where);
    void dragMove(const QPoint& where);
    /// Takes the drag away without dropping it.
    void dragLeave();
    /// Drops what dragEnter() is carrying. Does nothing without one.
    void dropAt(const QPoint& where);
    /// Lets bindings, queued task results and the render loop catch up.
    ///
    /// Drains the event queue rather than sleeping, and turns one frame so a
    /// layout pass has run -- a condition rather than a duration, which is what
    /// makes an assertion straight afterwards mean something on a loaded runner.
    /// `rounds` is how many times the queue is drained, because draining it can
    /// post more work. See MOLE-400.
    void settle(int rounds = 5);
    /// Spins until `predicate` holds, or the timeout expires.
    bool until(const std::function<bool()>& predicate, int timeoutMs = 10000);

    /// The thumbnail queue the window's own image provider is driving, so a test
    /// can hold a claim about the view and the queue together. Null before start().
    ThumbnailPump* thumbnails() const;

    /// Finds an item by objectName. Walks the visual tree, because
    /// QObject::findChild does not follow what Loader and SplitView build.
    QQuickItem* item(const QString& objectName) const;
    /// The named item inside `root`, for reaching into one delegate of a view
    /// rather than into the window. Static, because it walks what it is given.
    static QQuickItem* itemIn(QQuickItem* root, const QString& objectName);
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

    /// Whether a grab waits for the application to stop working first.
    ///
    /// It should, almost always. The task strip is in every picture and says how
    /// many jobs have finished, so a job that lands between one run's grab and the
    /// next turns "16 finished" into "17 finished" -- a picture rewritten for a
    /// reason that has nothing to do with the change being reviewed, and because the
    /// strip is always on screen it can happen to any picture at all rather than a
    /// nameable few. Waiting for the count to stop moving
    /// makes it "everything the walkthrough has run so far", which is fixed.
    ///
    /// `Working` is for the handful of pictures that are *of* something still
    /// going: a folder still loading, a CSV part-read, a transfer running. Waiting
    /// there would photograph the finished state and break the rule that a picture
    /// shows what the assertions just checked. See MOLE-255.
    enum class Settle {
        Idle,
        Working,
    };

    /// Renders the window. Empty when screenshots are disabled or grabbing
    /// failed, which the caller should treat as "skip", not as a failure.
    QImage grab(Settle mode = Settle::Idle);
    /// Renders and writes `<screenshotDirectory>/<name>.png`. Returns the path.
    QString screenshot(const QString& name, Settle mode = Settle::Idle);

private:
    /// Everything from building the controller to the window being exposed.
    /// Shared by start() and restart(), which differ only in whether the
    /// profile underneath is a new one.
    bool build(QString* errorOut);

    /// Turns one frame, so a layout pass has run before anything reads a
    /// position. See settle().
    void renderOneFrame();
    /// Rounds of a fixed wait, for the one caller that needs elapsed time: a
    /// picture. See the comment on the definition.
    void settleByTheClock(int rounds);

    /// Whether asking for a frame produces one. False after one attempt that did
    /// not, so a machine with no working surface pays the guard once.
    bool m_framesArrive = true;

    Options m_options;
    std::unique_ptr<PrivateProfile> m_profile;
    /// One or the other: a temporary directory for an ordinary run, a fixed path
    /// for a screenshot run. See Options::screenshotDirectory.
    std::unique_ptr<QTemporaryDir> m_fixture;
    QString m_fixedFixture;
    /// Where the sidebar's drives point. Its own directory rather than a corner
    /// of the fixture, because the fixture is what the listing tests count.
    std::unique_ptr<QTemporaryDir> m_drives;
    std::unique_ptr<AppController> m_app;
    std::unique_ptr<QQmlApplicationEngine> m_engine;
    QQuickWindow* m_window = nullptr;
    /// Owned by the engine, kept here so thumbnails() can reach the queue.
    ThumbnailImageProvider* m_thumbnails = nullptr;
    QString m_screenshotDirectory;
    /// The payload of a drag in flight. One at a time, because a pointer is one.
    std::unique_ptr<QMimeData> m_dragged;
};

} // namespace mole::test
