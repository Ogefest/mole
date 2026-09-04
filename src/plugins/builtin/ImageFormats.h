#pragma once

#include <QStringList>

namespace mole {

/// Every image suffix this build can decode, lower case.
///
/// Asked of Qt rather than hard-coded, because which formats exist depends on
/// which image plugins the build has -- and claiming one we cannot decode shows
/// an empty frame instead of the file's details.
///
/// **The same eight-line loop was written twice**, in the image preview provider
/// and in the thumbnailer, so a build where the two lists disagreed would offer
/// a preview of a file it could not draw a thumbnail of, or the other way round.
///
/// The listing's four hard-coded suffixes are a different question and stay
/// where they are: they decide which files get a *picture* in the icon column at
/// all, which is a choice about what a listing looks like rather than about what
/// Qt can read. Here rather than in core, which links no Qt Gui and is headless
/// on purpose. See MOLE-403.
QStringList imageSuffixes();

} // namespace mole
