#pragma once

#include "sdk/PluginServices.h"

#include <QString>
#include <QUrl>

namespace mole {

class FeatureController;

/// One kind of tab -- one workflow over files.
///
/// This is the extension point behind "every tab is a different way of working
/// with files". A duplicate finder, a metadata analyser, a bulk renamer, a diff
/// view: each is a class implementing this interface plus one QML file. The
/// shell asks the registry what exists, lists it in the new-tab menu, and loads
/// whatever viewSource() points at. It never learns what any feature does.
///
/// A feature is a factory, not a tab: createController() runs once per opened
/// tab, so ten browser tabs each get their own independent state.
class IFeature
{
public:
    virtual ~IFeature() = default;

    /// Stable, namespaced identifier -- "mole.browser", "org.example.duplicates".
    /// Used to persist open tabs across restarts, so never change it once
    /// released.
    virtual QString id() const = 0;

    /// Shown in the new-tab menu and used as a new tab's initial label.
    virtual QString title() const = 0;
    /// One line explaining what this tab is for.
    virtual QString description() const = 0;
    /// A glyph for the tab strip and menu.
    virtual QString iconText() const = 0;

    /// Ordering hint for the new-tab menu; lower comes first.
    virtual int sortOrder() const { return 100; }

    /// True when this tab is meaningless without something already selected.
    /// A preview needs a file; an alert list does not. The shell uses it to
    /// avoid offering a tab that would open onto nothing -- which reads as the
    /// feature being broken rather than as it being inapplicable.
    virtual bool needsContext() const { return false; }

    /// True when opening a new, empty tab of this kind is something somebody
    /// actually does -- a browser, a search. The File menu offers "New … tab"
    /// for exactly these, and for nothing else.
    ///
    /// False, the default, for both of the other two kinds, which is most
    /// features. One needs a subject: a preview needs a file, a bulk rename a
    /// selection, and a tab opened from nothing has nothing to show. The other is
    /// a standing tool that exists once -- the alerts list, the saved reports, the
    /// schedule -- where a second tab is a duplicate of the first and the entry
    /// reads better as its own name in the Workflows section than as something
    /// being created. See ADR-0003 for that split and ADR-0032 for this one.
    ///
    /// **A feature that answers false must be reachable some other way**: its own
    /// menu action, which puts it in the command palette too. That is not a
    /// suggestion — `everyFeatureIsReachableFromTheMenu` in `tst_AppIntegration`
    /// fails when a registered feature is named by no action at all.
    virtual bool opensFromNothing() const { return false; }

    /// QML component rendering the tab body. It is instantiated with a
    /// `controller` property holding the object from createController().
    /// Point it at a qrc: url shipped inside the plugin's own resources.
    virtual QUrl viewSource() const = 0;

    /// Fresh state for one newly opened tab. Ownership passes to `parent`.
    virtual FeatureController* createController(QObject* parent) = 0;
};

} // namespace mole
