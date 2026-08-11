#include "QmlAppHarness.h"

#include "plugins/builtin/BuiltinPlugin.h"
#include "ui/AppController.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_UNIX
#include <utime.h>
#endif

namespace mole::test {
namespace {

    /// How many frames grab() will look at before taking whatever it has.
    ///
    /// Each attempt costs one settle(2) -- about 40 ms -- so this is roughly
    /// eight hundred milliseconds, comfortably past the 220 ms of the longest
    /// transition in the window. A settled picture leaves after two attempts and
    /// never comes near it; the one picture that is deliberately of something
    /// still moving pays the whole cap, once. See grab().
    constexpr int kGrabAttempts = 20;

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

    // A drive list that came from the fixture rather than from whichever desk
    // this is running on. Every picture in the user guide is one of these
    // windows photographed, and the sidebar used to show the machine's own
    // volumes by name and capacity -- see the note in mountDefaultDrives().
    // Home is the fixture itself: the application starts there, so something
    // has to be mounted over it, and that is exactly what a home directory is.
    // Media is a second, empty drive so the list is not one row long.
    const QString media = QDir(m_drives->path()).filePath(QStringLiteral("media"));
    QDir().mkpath(media);
    qputenv("MOLE_DRIVES", QStringLiteral("Home=%1;Media=%2").arg(fixturePath(), media).toLocal8Bit());

    m_app = std::make_unique<AppController>();
    std::vector<std::unique_ptr<IPlugin>> builtIns;
    builtIns.push_back(std::make_unique<BuiltinPlugin>(startUri));

    QString error;
    if (!m_app->initialise(std::move(builtIns), &error))
        return fail(error);

    m_engine = std::make_unique<QQmlApplicationEngine>();
    m_engine->rootContext()->setContextProperty(QStringLiteral("App"), m_app.get());
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

bool QmlAppHarness::setModified(const QString& relativePath, const QDateTime& when)
{
    const QString target = QDir(fixturePath()).filePath(relativePath);

    // A directory cannot be opened as a QFile, and QFileDevice::setFileTime is
    // the only portable way Qt offers -- so folders go through utime(), which is
    // POSIX and is what the screenshot run uses. A folder whose date comes from
    // the clock is a folder whose date differs between two regenerations of the
    // same commit, which is the whole thing the fixed fixture name is for.
    if (QFileInfo(target).isDir()) {
#ifdef Q_OS_UNIX
        const time_t stamp = static_cast<time_t>(when.toSecsSinceEpoch());
        utimbuf times { stamp, stamp };
        return utime(QFile::encodeName(target).constData(), &times) == 0;
#else
        return true;
#endif
    }

    QFile file(target);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    return file.setFileTime(when, QFileDevice::FileModificationTime);
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

QPoint QmlAppHarness::centreOf(const QQuickItem* item) const
{
    if (!item || !m_window)
        return {};
    const QPointF centre = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    return centre.toPoint();
}

QQuickItem* QmlAppHarness::item(const QString& objectName) const
{
    if (!m_window)
        return nullptr;

    const std::function<QQuickItem*(QQuickItem*)> search = [&](QQuickItem* node) -> QQuickItem* {
        if (!node)
            return nullptr;
        if (node->objectName() == objectName)
            return node;
        const QList<QQuickItem*> children = node->childItems();
        for (QQuickItem* child : children) {
            if (QQuickItem* found = search(child))
                return found;
        }
        return nullptr;
    };
    return search(m_window->contentItem());
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
    return search(m_window->contentItem());
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

QImage QmlAppHarness::grab()
{
    if (!m_window)
        return {};

    // Two identical frames, rather than a fixed wait chosen to outlast whatever
    // the longest animation happens to be today. That number goes stale the
    // first time somebody writes a slower transition, and it had: every picture
    // of a dialog was taken 40 ms into the Material style's 220 ms enter
    // transition, at about 96% scale, with the body not yet opaque -- the file
    // listing showed through between a dialog's title bar and its footer, and
    // one picture had a file name legible *through* the dialog covering it.
    QImage previous;
    for (int attempt = 0; attempt < kGrabAttempts; ++attempt) {
        settle(2);
        QImage frame = m_window->grabWindow();
        if (frame.isNull())
            return frame;
        if (!previous.isNull() && frame == previous)
            return frame;
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

QString QmlAppHarness::screenshot(const QString& name)
{
    if (m_screenshotDirectory.isEmpty())
        return {};

    const QImage image = grab();
    if (image.isNull())
        return {};

    const QString path = QDir(m_screenshotDirectory).filePath(name + QStringLiteral(".png"));
    return image.save(path) ? path : QString();
}

} // namespace mole::test
