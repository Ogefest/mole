#pragma once

#include <QDateTime>
#include <QString>

namespace mole {

/// How long ago, in words.
///
/// The whole reason the index is safe to default to is that it admits its own
/// age, so this is said out loud rather than implied -- and said the same way
/// wherever it appears, which is why it is here rather than beside one caller.
/// The search form's coverage note and the list of indexes are the two.
QString ageInWords(const QDateTime& when);

} // namespace mole
