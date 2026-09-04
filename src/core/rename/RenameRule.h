#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace mole {

/// One step in a rename.
///
/// A list of independent steps rather than a form with eight fields, from the
/// start. It is the difference between adding a ninth operation and rewriting
/// the tool -- and it is also what makes each step testable on its own.
struct RenameRule
{
    enum class Kind {
        Replace, ///< find and replace, plainly or by pattern
        Case, ///< upper, lower, title, sentence
        Insert, ///< put text at a position
        Remove, ///< take out a range of characters
        Strip, ///< take out a class of characters
        Number, ///< a counter, with start, step and padding
        Affix, ///< prefix and suffix
        Extension ///< change or normalise the suffix
    };

    /// Which part of the name a step touches. Most steps want the stem: a rule
    /// that upper-cases a name should not turn ".TXT" into something no tool
    /// recognises.
    /// These three are stored as their own numbers, and `fromJson()` checks a
    /// value against the range before casting -- so **the last enumerator of
    /// each is the range check**. Add one at the end; do not add one in the
    /// middle, which would change what an already-saved rule means.
    enum class Scope { Stem, Extension, WholeName };

    enum class CaseStyle { Upper, Lower, Title, Sentence };
    enum class StripClass { Digits, Punctuation, Whitespace, Accents, NonAscii };

    Kind kind = Kind::Replace;
    Scope scope = Scope::Stem;
    bool enabled = true;

    // Replace
    QString find;
    QString replaceWith;
    bool useRegex = false;
    bool caseSensitive = false;

    // Case
    CaseStyle caseStyle = CaseStyle::Lower;

    // Insert / Remove
    /// Negative counts from the end, so "the last three characters" needs no
    /// separate rule.
    int position = 0;
    int length = 0;
    QString text;

    // Strip
    StripClass stripClass = StripClass::Digits;

    // Number
    int start = 1;
    int step = 1;
    int padding = 3;
    /// Put the counter here. Negative counts from the end.
    int numberAt = -1;
    QString numberSeparator;

    // Affix
    QString prefix;
    QString suffix;

    // Extension
    QString newExtension;

    /// A short human sentence, for the rule list: "replace 'IMG' with 'Photo'".
    QString describe() const;

    QJsonObject toJson() const;
    static RenameRule fromJson(const QJsonObject& json);

    static QString kindToString(Kind kind);
    static Kind kindFromString(const QString& text);
    static QString kindLabel(Kind kind);
    /// Every kind, in the order the interface offers them.
    static QList<Kind> allKinds();
};

} // namespace mole
