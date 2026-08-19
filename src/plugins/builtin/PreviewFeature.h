#pragma once

#include "sdk/FeatureController.h"
#include "sdk/IFeature.h"
#include "sdk/IPreviewProvider.h"

#include "core/vfs/FileEntry.h"

#include <QPointer>
#include <QUrl>
#include <QVariantList>

namespace mole {

class ListDirectoryTask;
class ReadRangeTask;
class ReadMetadataTask;

/// A tab that shows one file at a time.
///
/// Named for the tab, not the viewer: the SDK's PreviewController is the base
/// an individual viewer subclasses, and this owns one of those at a time.
///
/// It owns two things the individual viewers do not: which file is being shown,
/// and the list of its neighbours so the arrow keys can step through the folder
/// without going back to the browser. Everything about *how* a file looks comes
/// from an IPreviewProvider, so a plugin adding a viewer needs to touch nothing
/// here.
class PreviewTabController final : public FeatureController
{
    Q_OBJECT
    Q_PROPERTY(QString currentUri READ currentUri NOTIFY currentChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY currentChanged)
    Q_PROPERTY(QString folderPath READ folderPath NOTIFY currentChanged)
    Q_PROPERTY(QString viewerName READ viewerName NOTIFY currentChanged)
    /// QML component for the current file, empty when nothing is open.
    Q_PROPERTY(QUrl viewSource READ viewSource NOTIFY currentChanged)
    /// The object that component binds to. Owned by this controller.
    Q_PROPERTY(QObject* viewer READ viewer NOTIFY currentChanged)
    /// True while the head of the file is being read to find out what it is, when
    /// there is no viewer yet and none has been ruled out. The view has to know
    /// the difference: "nothing can show this file" would be a lie for the moment
    /// the answer takes to arrive. See ADR-0033.
    Q_PROPERTY(bool identifying READ isIdentifying NOTIFY currentChanged)
    /// What can be chosen about how this file is shown, for the strip to render:
    /// one map per option with `key`, `title`, `choices` and `chosen`. Empty for
    /// most files. See docs/adr/0006-preview-options-and-preferences.md.
    Q_PROPERTY(QVariantList viewerOptions READ viewerOptions NOTIFY currentChanged)
    /// The details panel: whether it is open, what is in it, and whether the
    /// readers are still working. Filled from the metadata registry rather than
    /// by any viewer, so every viewer has it and none of them knows it exists --
    /// the same argument ADR-0006 makes for declared options.
    /// See docs/adr/0034-what-a-file-says-about-itself.md.
    Q_PROPERTY(bool detailsOpen READ isDetailsOpen NOTIFY detailsChanged)
    Q_PROPERTY(bool detailsLoading READ isDetailsLoading NOTIFY detailsChanged)
    /// One map per fact, with `label`, `value` and `startsBlock` -- true on the
    /// first row of each reader's contribution, which is where the drawer draws
    /// a line. The order is the readers' priority order; nothing is regrouped.
    Q_PROPERTY(QVariantList details READ details NOTIFY detailsChanged)
    /// How wide the drawer is, remembered across restarts. A choice about the
    /// person's screen, like whether it is open at all.
    Q_PROPERTY(int detailsWidth READ detailsWidth NOTIFY detailsChanged)
    Q_PROPERTY(int position READ position NOTIFY currentChanged)
    Q_PROPERTY(int siblingCount READ siblingCount NOTIFY currentChanged)
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY currentChanged)
    Q_PROPERTY(bool canGoPrevious READ canGoPrevious NOTIFY currentChanged)

public:
    explicit PreviewTabController(PluginServices services, QObject* parent = nullptr);
    ~PreviewTabController() override;

    QString currentUri() const { return m_current.uri.toString(); }
    QString fileName() const { return m_current.name; }
    QString folderPath() const;
    QString viewerName() const { return m_viewerName; }
    /// Which provider is showing the file. The display name is what the strip
    /// shows; this is the stable id, for whoever needs to know exactly.
    QString providerId() const { return m_providerId; }
    QVariantList viewerOptions() const { return m_viewerOptions; }
    /// Chooses one, remembers it for this file type, and shows the result now.
    Q_INVOKABLE void chooseViewerOption(const QString& key, const QString& value);
    QUrl viewSource() const { return m_viewSource; }
    QObject* viewer() const;
    bool isIdentifying() const { return !m_sniff.isNull(); }
    /// One-based, for "3 of 17".
    int position() const;
    int siblingCount() const { return static_cast<int>(m_siblings.size()); }
    bool canGoNext() const { return position() > 0 && position() < siblingCount(); }
    bool canGoPrevious() const { return position() > 1; }

    bool isDetailsOpen() const { return m_detailsOpen; }
    bool isDetailsLoading() const { return !m_details.isNull(); }
    QVariantList details() const { return m_detailFacts; }
    int detailsWidth() const { return m_detailsWidth; }
    /// Opens or closes the drawer, remembers the answer -- **once, for every
    /// file** rather than per file type -- and reads the facts when it is
    /// opened. Nothing is read for a drawer nobody opened: the cost of an
    /// expensive reader falls on whoever asked for it.
    ///
    /// Not ADR-0006's per-suffix key, deliberately. That one is right for
    /// *render this .html as a page*, which is a choice about a file type;
    /// whether a drawer is open is a choice about the person and their screen,
    /// and having it appear for a .jpg and vanish for a .png is the surprise.
    Q_INVOKABLE void setDetailsOpen(bool open);
    /// Remembers how wide the reader left it. Called when the divider is
    /// released rather than as it moves: a preference is a file on disk.
    Q_INVOKABLE void setDetailsWidth(int width);
    /// Puts every row on the clipboard as `label: value` lines. The second thing
    /// anybody wants after one value is all of them.
    Q_INVOKABLE void copyDetails();
    /// Read the facts although the drawer is shut, because the viewer on screen
    /// is showing them itself -- which the information viewer does, its content
    /// being the facts. The cost rule survives: something is about to show them.
    Q_INVOKABLE void requestDetails();

    /// Shows `uri` and loads its folder in the background so the arrows work.
    Q_INVOKABLE void open(const QString& uri);
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();

    QVariantMap saveState() const override;
    void restoreState(const QVariantMap& state) override;

signals:
    void currentChanged();
    void detailsChanged();

private:
    void showEntry(const FileEntry& entry);
    /// The one member of a file compressed on its own, or an invalid entry.
    ///
    /// A `.gz` that is not a tarball is a wrapper around exactly one file, and
    /// what a reader pressing F3 wanted is what is inside it. ADR-0033 already
    /// says the first answer about a file can be wrong and its contents settle
    /// it; this is a third reason, and not about identification: the contents are
    /// inside a wrapper, so what should be resolved is the member. Since MOLE-216
    /// the member has an address of its own, so the whole of it is an entry swap
    /// followed by the ordinary lookup -- and every viewer Mole already has works
    /// through it, a `.gz` of a CSV being a table and of a PNG being the picture.
    ///
    /// Mounts the wrapper internally, because a viewer reads by resolving a uri.
    /// The mount goes when the file being shown does -- see releaseMemberMount().
    FileEntry singleCompressedMember(const FileEntry& entry);
    /// Unmounts the wrapper a substituted member was read through, if there is
    /// one. Called before every file change and on the way out, so a walk along a
    /// folder of `.gz` files leaves nothing behind.
    void releaseMemberMount();
    /// Reads the head of the current file, puts what it is in
    /// `FileEntry::mimeType`, and asks the registry again. Only for a file whose
    /// name got no further than the fallback tier -- which is where the answer
    /// can still change.
    void identifyThenShow();
    /// Builds the viewer for the current entry, or says nothing can show it.
    void installViewer(IPreviewProvider* provider);
    /// Starts the readers for the current file, if the panel is open and there
    /// is anything to ask.
    void readDetails();
    void loadSiblings(const VfsUri& directory, const VfsUri& select);
    void step(int delta);

    PluginServices m_services;
    /// The file in the folder: what the arrows step through, what the session
    /// records, and what the subtitle names.
    FileEntry m_current;
    /// What the viewer was given, which is the same thing unless a wrapper was
    /// opened -- then it is the member inside it. Every question about *what is
    /// on screen* is asked of this one: which provider, which options, which
    /// facts.
    FileEntry m_showing;
    /// The internal mount a substituted member is read through, or empty.
    QString m_memberMountId;
    /// Who to give it back to. Held as a QPointer and not as the raw pointer in
    /// PluginServices because an application shutting down destroys the manager
    /// before the tabs it owns, and this is released from a destructor.
    QPointer<VfsManager> m_memberMountOwner;
    /// Files only: stepping into a directory from a preview makes no sense.
    FileEntryList m_siblings;
    QString m_viewerName;
    QVariantList m_viewerOptions;
    /// The provider showing the current file, for the preference keys.
    QString m_providerId;

    // The two halves of remembering a choice: where it is written, and what it
    // says when nothing has been written yet.
    QString preferenceKey(const QString& optionKey, const FileEntry& entry) const;
    QString rememberedChoice(const ViewerOption& option, const FileEntry& entry) const;
    QUrl m_viewSource;
    QPointer<QObject> m_viewer;
    QPointer<ListDirectoryTask> m_listing;
    QPointer<ReadRangeTask> m_sniff;
    QPointer<ReadMetadataTask> m_details;

    /// The head the content pass read, kept so opening the panel costs no
    /// further read for a file that was identified from its bytes.
    QByteArray m_head;
    bool m_detailsOpen = false;
    int m_detailsWidth = 320;
    QVariantList m_detailFacts;
};

class PreviewFeature final : public IFeature
{
public:
    explicit PreviewFeature(PluginServices services);

    QString id() const override { return QStringLiteral("mole.preview"); }
    QString title() const override { return QStringLiteral("Preview"); }
    QString description() const override
    {
        return QStringLiteral("Look inside one file, and step through the folder with the arrows.");
    }
    QString iconText() const override { return QStringLiteral("\U0001F441"); }
    bool needsContext() const override { return true; }
    int sortOrder() const override { return 25; }

    QUrl viewSource() const override;
    FeatureController* createController(QObject* parent) override;

private:
    PluginServices m_services;
};

} // namespace mole
