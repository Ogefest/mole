#include "QmlAppHarness.h"

#include "plugins/builtin/BuiltinPlugin.h"
#include "ui/AppController.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

namespace mole::test {

QmlAppHarness::QmlAppHarness() = default;

QmlAppHarness::~QmlAppHarness()
{
    stop();
}

QString QmlAppHarness::fixturePath() const
{
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
    m_fixture = std::make_unique<QTemporaryDir>();
    if (!m_profile->isValid() || !m_fixture->isValid())
        return fail(QStringLiteral("could not create temporary directories"));

    m_screenshotDirectory = options.screenshotDirectory;
    if (!m_screenshotDirectory.isEmpty())
        QDir().mkpath(m_screenshotDirectory);

    const QString startUri = options.startUri.isEmpty() ? fixtureUri() : options.startUri;

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
    m_engine.reset();
    m_window = nullptr;
    m_app.reset();
    m_fixture.reset();
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
    settle(2);
    return m_window->grabWindow();
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
