#include "QmlAppHarness.h"

#include "ThumbnailImageProvider.h"
#include "plugins/builtin/BuiltinPlugin.h"
#include "ui/AppController.h"
#include "ui/models/TaskListModel.h"

#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <algorithm>

#ifdef Q_OS_UNIX
#endif

namespace mole::test {
namespace {

    /// How many frames grab() will look at before taking whatever it has.
    ///
    /// Each attempt costs one settle(2) -- about 40 ms -- so this is roughly two
    /// and a half seconds. Eight hundred milliseconds was the old figure, chosen
    /// against the 220 ms of the longest *transition*; what it missed is a
    /// scrollbar, which does not transition but fades itself out after a delay
    /// once nothing is touching it. A list of two items in the compress dialog
    /// briefly wants one, and the faint remains of it were in the picture on some
    /// runs and not others. A settled picture leaves after three attempts and
    /// never comes near this; the three that are deliberately of something still
    /// moving pay the whole cap. See grab() and MOLE-255.
    constexpr int kGrabAttempts = 60;

} // namespace

QmlAppHarness::QmlAppHarness() = default;

QmlAppHarness::~QmlAppHarness()
{
    stop();
}

QString QmlAppHarness::fixturePath() const
{
    if (!m_fixedFixture.isEmpty())
        return m_fixedFixture;
    return m_fixture ? m_fixture->path() : QString();
}

QString QmlAppHarness::fixtureUri() const
{
    return VfsUri::fromLocalPath(fixturePath()).toString();
}

bool QmlAppHarness::start(const Options& options, QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    // A private profile per run: the user's real session, bookmarks, index,
    // analysis history, schedule and alerts must never be touched -- the
    // scheduler starts on its own, and it would run their jobs.
    m_profile = std::make_unique<PrivateProfile>();
    m_drives = std::make_unique<QTemporaryDir>();
    if (!m_profile->isValid() || !m_drives->isValid())
        return fail(QStringLiteral("could not create temporary directories"));

    m_screenshotDirectory = options.screenshotDirectory;
    if (m_screenshotDirectory.isEmpty()) {
        m_fixture = std::make_unique<QTemporaryDir>();
        if (!m_fixture->isValid())
            return fail(QStringLiteral("could not create a temporary directory"));
    } else {
        QDir().mkpath(m_screenshotDirectory);
        // A name a reader recognises, in place of `mole-tests-wAYrZa`. Emptied
        // first rather than reused: a leftover from a previous run would appear
        // in the pictures as a file nobody wrote.
        m_fixedFixture = QDir(QDir::tempPath()).filePath(QStringLiteral("mole-guide"));
        QDir(m_fixedFixture).removeRecursively();
        if (!QDir().mkpath(m_fixedFixture))
            return fail(QStringLiteral("could not create the fixture directory"));
    }

    m_options = options;
    return build(errorOut);
}

bool QmlAppHarness::restart(QString* errorOut)
{
    m_engine.reset();
    m_thumbnails = nullptr; // the engine owned it
    m_window = nullptr;
    m_app.reset();
    return build(errorOut);
}

bool QmlAppHarness::build(QString* errorOut)
{
    const auto fail = [errorOut](const QString& message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    const Options& options = m_options;
    const QString startUri = options.startUri.isEmpty() ? fixtureUri() : options.startUri;

    // Point the host at the plugins this build produced, so a harness test sees
    // the same drives a real run does. It matters more than it looks: the network
    // backends are a loadable plugin rather than compiled in, so without this the
    // drives dialog would come up with nothing to offer and the tests that check
    // it would be checking an empty application.
    qputenv("MOLE_PLUGIN_PATH", QByteArray(MOLE_TEST_PLUGIN_DIR));

    // A text cursor that never blinks. Zero means "do not flash" to Qt, and a
    // window photographed with a caret in it otherwise gives one picture with the
    // caret drawn and the next without -- `report|` and `report`, the same state
    // twice, differing. grab() cannot help: a blink is a change that never stops,
    // so there is no pair of identical frames to wait for. See MOLE-255.
    QGuiApplication::styleHints()->setCursorFlashTime(0);

    // And nothing that breathes for ever. See AppController::stillPictures.
    if (!options.screenshotDirectory.isEmpty())
        qputenv("MOLE_STILL_PICTURES", "1");

    // A shell with no rc files and a prompt of our own.
    //
    // The terminal panel starts `$SHELL -i`, so it used to be whoever ran the
    // suite: their shell, their rc files, and their prompt. Two things came of
    // that. **A picture in a public repository carried a real user name and a real
    // machine name** -- `lg@lg-ThinkStation-P360-Tower` was committed in
    // `10-terminal.png`, which is exactly what CLAUDE.md forbids anywhere in this
    // checkout. And the prompt's length decided whether the echoed command line
    // wrapped, so the panel had scrolled by one line in one run and not the next.
    //
    // A wrapper rather than `SHELL=/bin/bash`, because the panel passes only `-i`
    // and there is no way to add `--norc` from here. `\w` keeps the folder in the
    // prompt, which is the whole claim the picture is evidence for. See MOLE-255.
    const QString shell = m_profile->filePath(QStringLiteral("guide-shell"));
    {
        QFile wrapper(shell);
        if (wrapper.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            wrapper.write("#!/bin/sh\n"
                          "# Written by QmlAppHarness. No rc files, so the prompt is the one\n"
                          "# exported below rather than the one belonging to whoever ran this.\n"
                          "exec /bin/bash --norc --noprofile -i\n");
            wrapper.close();
            wrapper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            qputenv("SHELL", shell.toLocal8Bit());
            qputenv("PS1", "mole:\\w\\$ ");
        }
    }

    // A drive list that came from the fixture rather than from whichever desk
    // this is running on. Every picture in the user guide is one of these
    // windows photographed, and the sidebar used to show the machine's own
    // volumes by name and capacity -- see the note in mountDefaultDrives().
    // Home is the fixture itself: the application starts there, so something
    // has to be mounted over it, and that is exactly what a home directory is.
    // Media is a second, empty drive so the list is not one row long.
    //
    // Each one is told what to report having, rather than being measured: both
    // sit on whichever disk this is running on, so the sidebar used to show the
    // real free space -- the same figure twice, and a different figure every few
    // minutes. Thirty-nine of the guide's fifty-three pictures changed between two
    // regenerations because of it. Invented numbers, a plausible size for each,
    // and different from each other so the two rows do not look like a bug.
    // See MOLE-255.
    const QString media = QDir(m_drives->path()).filePath(QStringLiteral("media"));
    QDir().mkpath(media);
    constexpr qint64 kGiB = 1024LL * 1024 * 1024;
    qputenv("MOLE_DRIVES",
        QStringLiteral("Home=%1|%2:%3;Media=%4|%5:%6")
            .arg(fixturePath())
            .arg(347LL * kGiB / 4) // 86,75 GiB free
            .arg(238LL * kGiB)
            .arg(media)
            .arg(1451LL * kGiB / 1000) // 1,45 GiB free
            .arg(29LL * kGiB / 2)
            .toLocal8Bit());

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(startUri));

    QString error;
    if (!m_app->initialise(std::move(builtIns), &error))
        return fail(error);

    m_engine = std::make_unique<QQmlApplicationEngine>();
    m_engine->rootContext()->setContextProperty(QStringLiteral("App"), m_app.get());
    // The same provider the window registers, from the same library: a provider
    // wired up only in main.cpp is a provider no test can be wrong about.
    // Kept, so a test can ask the queue what it is doing. The engine owns it.
    m_thumbnails = new mole::ThumbnailImageProvider(m_app->services());
    m_engine->addImageProvider(mole::ThumbnailKey::providerName(), m_thumbnails);
    m_engine->load(QUrl(QStringLiteral("qrc:/qt/qml/Mole/ui/Main.qml")));

    if (m_engine->rootObjects().isEmpty())
        return fail(QStringLiteral("the user interface failed to load"));

    m_window = qobject_cast<QQuickWindow*>(m_engine->rootObjects().first());
    if (!m_window)
        return fail(QStringLiteral("the root object is not a window"));

    m_window->resize(options.windowWidth, options.windowHeight);
    m_window->show();
    if (!QTest::qWaitForWindowExposed(m_window))
        return fail(QStringLiteral("the window was never exposed"));

    settle();
    return true;
}

void QmlAppHarness::stop()
{
    // Unset rather than left pointing at a directory that is about to go: a
    // stale value would send the next application in this process to nowhere.
    qunsetenv("MOLE_DRIVES");
    m_engine.reset();
    m_thumbnails = nullptr; // the engine owned it
    m_window = nullptr;
    m_app.reset();
    if (!m_fixedFixture.isEmpty()) {
        QDir(m_fixedFixture).removeRecursively();
        m_fixedFixture.clear();
    }
    m_fixture.reset();
    m_drives.reset();
    m_profile.reset();
}

bool QmlAppHarness::makeDirs(const QString& relativePath)
{
    return QDir(fixturePath()).mkpath(relativePath);
}

bool QmlAppHarness::writeFile(const QString& relativePath, const QByteArray& contents)
{
    const QString target = QDir(fixturePath()).filePath(relativePath);
    const QFileInfo info(target);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath()))
        return false;

    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(contents) == contents.size();
}

bool QmlAppHarness::writeSparseFile(const QString& relativePath, qint64 bytes)
{
    const QString target = QDir(fixturePath()).filePath(relativePath);
    const QFileInfo info(target);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath()))
        return false;

    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.resize(bytes);
}

bool QmlAppHarness::fixDatesUnder(const QString& relativePath, const QDate& epoch)
{
    const QString root = relativePath.isEmpty() ? fixturePath() : QDir(fixturePath()).filePath(relativePath);
    if (!QFileInfo(root).isDir())
        return false;

    // FNV-1a, so the same path gives the same date on every machine and in every
    // Qt version. Spread over about two years, which is what makes a date column
    // worth having and a sort by age worth showing.
    const auto dateFor = [&epoch](const QString& path) {
        quint32 hash = 2166136261u;
        for (const char byte : path.toUtf8())
            hash = (hash ^ static_cast<quint8>(byte)) * 16777619u;
        const int daysAgo = static_cast<int>(hash % 760u);
        const int hour = static_cast<int>((hash / 760u) % 24u);
        const int minute = static_cast<int>((hash / 18240u) % 60u);
        return QDateTime(epoch.addDays(-daysAgo), QTime(hour, minute));
    };

    // The end of the epoch day. Anything at or before it was stamped on purpose
    // and is left alone; anything after it came from the clock.
    const QDateTime deliberate(epoch, QTime(23, 59, 59));

    QStringList directories;
    bool ok = true;
    QDirIterator walk(
        root, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        const QString absolute = walk.next();
        const QFileInfo info(absolute);
        if (absolute.contains(QStringLiteral("/.git/")) || info.fileName() == QStringLiteral(".git"))
            continue;
        if (info.isDir()) {
            directories.append(absolute);
            continue;
        }
        if (info.lastModified() <= deliberate)
            continue;
        ok = setModifiedTime(absolute, dateFor(QDir(fixturePath()).relativeFilePath(absolute))) && ok;
    }

    // Deepest first: stamping a file leaves its folder alone, but a folder
    // stamped before one of its own subfolders would be undone by nothing --
    // this only keeps the order honest if the platform ever changes that.
    std::sort(directories.begin(), directories.end(),
        [](const QString& a, const QString& b) { return a.size() > b.size(); });
    for (const QString& directory : directories) {
        if (QFileInfo(directory).lastModified() <= deliberate)
            continue;
        ok = setModifiedTime(directory, dateFor(QDir(fixturePath()).relativeFilePath(directory))) && ok;
    }
    return ok;
}

bool QmlAppHarness::setModified(const QString& relativePath, const QDateTime& when)
{
    const QString target = QDir(fixturePath()).filePath(relativePath);

    // A folder whose date comes from the clock is a folder whose date differs
    // between two regenerations of the same commit, which is the whole thing the
    // fixed fixture name is for. Folders need utime(); the shared helper knows.
    return setModifiedTime(target, when);
}

void QmlAppHarness::settle(int rounds)
{
    for (int i = 0; i < rounds; ++i)
        QTest::qWait(20);
}

bool QmlAppHarness::until(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (predicate())
            return true;
        QTest::qWait(15);
    }
    return predicate();
}

void QmlAppHarness::key(int key, Qt::KeyboardModifiers modifiers)
{
    if (!m_window)
        return;
    QTest::keyClick(m_window, static_cast<Qt::Key>(key), modifiers);
    settle(3);
}

void QmlAppHarness::type(const QString& text)
{
    // Character by character, because that is how a filter that reacts to the
    // first keystroke has to be exercised.
    for (const QChar c : text) {
        QTest::keyClick(m_window, c.toLatin1(), Qt::NoModifier);
        settle(1);
    }
}

void QmlAppHarness::press(const QPoint& where, Qt::MouseButton button)
{
    if (!m_window)
        return;
    QTest::mousePress(m_window, button, Qt::NoModifier, where);
    settle(1);
}

void QmlAppHarness::moveTo(const QPoint& where)
{
    if (!m_window)
        return;
    // QTest remembers which buttons are down, so a move between a press and a
    // release carries the button state a drag needs.
    QTest::mouseMove(m_window, where);
    settle(1);
}

void QmlAppHarness::release(const QPoint& where, Qt::MouseButton button)
{
    if (!m_window)
        return;
    QTest::mouseRelease(m_window, button, Qt::NoModifier, where);
    settle(2);
}

void QmlAppHarness::click(const QPoint& where)
{
    if (!m_window)
        return;
    QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, where);
    settle(3);
}

bool QmlAppHarness::clickOn(QQuickItem* item)
{
    if (!item || !m_window)
        return false;

    // Twenty rounds is about eight hundred milliseconds. A settled item leaves
    // after two; nothing in this window moves for longer than that except the
    // animations grab() is about, and none of those is something a test clicks.
    QRectF previous;
    for (int attempt = 0; attempt < 20; ++attempt) {
        const QRectF here(item->mapToScene(QPointF(0, 0)), QSizeF(item->width(), item->height()));
        if (!here.isEmpty() && here == previous) {
            click(here.center().toPoint());
            return true;
        }
        previous = here;
        settle(1);
    }
    return false;
}

void QmlAppHarness::doubleClick(const QPoint& where)
{
    if (!m_window)
        return;
    QTest::mouseDClick(m_window, Qt::LeftButton, Qt::NoModifier, where);
    settle(3);
}

void QmlAppHarness::dragFrom(const QPoint& from, const QPoint& to)
{
    if (!m_window)
        return;

    // Comfortably past whatever the platform calls a drag: the threshold is a
    // style hint, so a test that moved exactly that far would be asserting the
    // hint rather than the behaviour.
    const int threshold = QGuiApplication::styleHints()->startDragDistance();
    const QPoint destination = to.isNull() ? from + QPoint(0, threshold * 3) : to;

    press(from);
    // In steps, because one jump can be delivered as a single move that a
    // handler treats as a teleport rather than as a drag.
    for (int step = 1; step <= 4; ++step)
        moveTo(from + (destination - from) * step / 4);
}

void QmlAppHarness::dragEnter(const QStringList& localPaths, const QPoint& where)
{
    if (!m_window)
        return;

    QList<QUrl> urls;
    urls.reserve(localPaths.size());
    for (const QString& path : localPaths)
        urls.append(QUrl::fromLocalFile(path));

    m_dragged = std::make_unique<QMimeData>();
    m_dragged->setUrls(urls);

    QDragEnterEvent enter(
        where, Qt::CopyAction | Qt::MoveAction, m_dragged.get(), Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window, &enter);
    settle(2);
}

void QmlAppHarness::dragMove(const QPoint& where)
{
    if (!m_window || !m_dragged)
        return;
    QDragMoveEvent move(
        where, Qt::CopyAction | Qt::MoveAction, m_dragged.get(), Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window, &move);
    settle(2);
}

void QmlAppHarness::dragLeave()
{
    if (!m_window || !m_dragged)
        return;
    QDragLeaveEvent leave;
    QCoreApplication::sendEvent(m_window, &leave);
    settle(2);
    m_dragged.reset();
}

void QmlAppHarness::dropAt(const QPoint& where)
{
    if (!m_window || !m_dragged)
        return;
    QDropEvent drop(where, Qt::CopyAction | Qt::MoveAction, m_dragged.get(), Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_window, &drop);
    settle(3);
    m_dragged.reset();
}

QPoint QmlAppHarness::centreOf(const QQuickItem* item) const
{
    if (!item || !m_window)
        return {};
    const QPointF centre = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    return centre.toPoint();
}

ThumbnailPump* QmlAppHarness::thumbnails() const
{
    return m_thumbnails ? m_thumbnails->pump() : nullptr;
}

QQuickItem* QmlAppHarness::itemIn(QQuickItem* root, const QString& objectName)
{
    if (!root)
        return nullptr;
    if (root->objectName() == objectName)
        return root;
    const QList<QQuickItem*> children = root->childItems();
    for (QQuickItem* child : children) {
        if (QQuickItem* found = itemIn(child, objectName))
            return found;
    }
    return nullptr;
}

QQuickItem* QmlAppHarness::item(const QString& objectName) const
{
    return m_window ? itemIn(m_window->contentItem(), objectName) : nullptr;
}

QList<QQuickItem*> QmlAppHarness::items(const QString& objectName) const
{
    QList<QQuickItem*> found;
    if (!m_window)
        return found;

    const std::function<void(QQuickItem*)> collect = [&](QQuickItem* node) {
        if (!node)
            return;
        if (node->objectName() == objectName)
            found.append(node);
        const QList<QQuickItem*> children = node->childItems();
        for (QQuickItem* child : children)
            collect(child);
    };
    collect(m_window->contentItem());
    return found;
}

QObject* QmlAppHarness::object(const QString& objectName) const
{
    if (!m_window)
        return nullptr;

    // QObject::findChild on the window finds nothing: QML does not parent items
    // into the window's QObject tree, so the two hierarchies do not line up.
    // The visual tree is the one that reflects the interface -- and a popup,
    // which is absent from it, hangs off the QObject children of the item that
    // declared it.
    const std::function<QObject*(QQuickItem*)> search = [&](QQuickItem* node) -> QObject* {
        if (!node)
            return nullptr;
        if (node->objectName() == objectName)
            return node;

        const QObjectList siblings = node->children();
        for (QObject* child : siblings) {
            if (child->objectName() == objectName)
                return child;
        }

        const QList<QQuickItem*> children = node->childItems();
        for (QQuickItem* child : children) {
            if (QObject* found = search(child))
                return found;
        }
        return nullptr;
    };
    if (QObject* found = search(m_window->contentItem()))
        return found;

    // And then the whole object tree from the root, which reaches what the visual
    // walk above cannot: a submenu is a QObject child of the menu that declared
    // it, two levels off the visual tree rather than one, so `menuView` was
    // invisible here while `appMenu` was not.
    if (QObject* root = m_engine ? m_engine->rootObjects().value(0) : nullptr)
        return root->findChild<QObject*>(objectName);
    return nullptr;
}

QString QmlAppHarness::focusChain() const
{
    if (!m_window)
        return {};

    QStringList chain;
    for (QQuickItem* node = m_window->activeFocusItem(); node; node = node->parentItem()) {
        QString name = QString::fromLatin1(node->metaObject()->className());
        name.remove(QRegularExpression(QStringLiteral("_QMLTYPE_.*")));
        if (!node->objectName().isEmpty())
            name += QStringLiteral("[%1]").arg(node->objectName());
        chain.prepend(name);
    }
    return chain.join(QStringLiteral(" > "));
}

QImage QmlAppHarness::grab(Settle mode)
{
    if (!m_window)
        return {};

    // Nothing still working, unless the picture is of something working.
    //
    // The task strip is in every one of the guide's pictures and says how many
    // jobs have finished. A job landing between one run's grab and the next turned
    // "16 finished" into "17 finished" -- and because the strip is always on
    // screen, that could rewrite any picture at all rather than a nameable few.
    // Waiting for the count to stop moving makes it "everything run so far",
    // which the walkthrough fixes. Bounded, because a run that never goes quiet
    // must give back a picture rather than hang. See MOLE-255.
    if (mode == Settle::Idle && m_app && m_app->tasks())
        until([this] { return m_app->tasks()->activeCount() == 0; }, 15000);

    // Two identical frames, rather than a fixed wait chosen to outlast whatever
    // the longest animation happens to be today. That number goes stale the
    // first time somebody writes a slower transition, and it had: every picture
    // of a dialog was taken 40 ms into the Material style's 220 ms enter
    // transition, at about 96% scale, with the body not yet opaque -- the file
    // listing showed through between a dialog's title bar and its footer, and
    // one picture had a file name legible *through* the dialog covering it.
    // Three in a row rather than two.
    //
    // Two was not enough at the tail of an animation. The Material style fades a
    // text field's focus underline in over about a quarter of a second, and the
    // last steps of that fade round to the same pixels -- so a pair of identical
    // frames could be found while the underline was still arriving, and the
    // picture came out with a half-drawn line in it. Once, and not the next time:
    // `11-drives` differed between two regenerations in exactly those 415 pixels
    // and nowhere else. A third frame costs 40 ms per picture. See MOLE-255.
    QImage previous;
    int matches = 0;
    for (int attempt = 0; attempt < kGrabAttempts; ++attempt) {
        settle(2);
        QImage frame = m_window->grabWindow();
        if (frame.isNull())
            return frame;
        if (!previous.isNull() && frame == previous) {
            if (++matches >= 2)
                return frame;
        } else {
            matches = 0;
        }
        previous = std::move(frame);
    }

    // The cap, and it is reached on purpose. `02b-preview-csv-loading` is
    // deliberately a load in progress and its BusyIndicator never stops turning,
    // so that picture can never have two identical frames: hitting the cap is the
    // correct outcome there rather than a failure, which is why this hands back
    // the last frame instead of complaining. Anything else reaching it is a
    // window that will not settle, and the picture will show why.
    return previous;
}

QString QmlAppHarness::screenshot(const QString& name, Settle mode)
{
    if (m_screenshotDirectory.isEmpty())
        return {};

    const QImage image = grab(mode);
    if (image.isNull())
        return {};

    const QString path = QDir(m_screenshotDirectory).filePath(name + QStringLiteral(".png"));
    return image.save(path) ? path : QString();
}

} // namespace mole::test
