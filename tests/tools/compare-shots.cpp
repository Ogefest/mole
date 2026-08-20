// Compares two directories of the guide's pictures, and says whether a
// regeneration would be reviewable.
//
// `make guide-images` used to rewrite most of the guide whether or not anything
// had changed -- fifty of fifty-three files over an unchanged tree. A ticket that
// moved one thing on screen produced a diff of fifty binary files, of which one
// was the change, so nobody could tell whether the picture that mattered had moved
// and a real regression in an unrelated picture was invisible. MOLE-255 is about
// making that diff mean something.
//
// **Byte-identical is not the bar, and measuring is why.** Once the free-space
// figures, the clock, the ordering, the caret and the task strip were fixed, the
// remaining differences were one to five levels out of 255, in a few dozen pixels,
// in pictures whose content was letter-for-letter the same -- the scene graph does
// not render a given frame to identical bytes twice. So this compares what an eye
// can see: a pixel counts as different when a channel differs by more than
// `--tolerance`, and a picture counts as changed when more than `--pixels` of them
// do. Both are printed, so a run says what it measured rather than only whether it
// liked it.
//
// Qt rather than ImageMagick or a Python library: Qt is already the dependency, so
// this needs nothing installed that building Mole did not already need.
//
// Usage:
//   compare-shots <before> <after> [--tolerance N] [--pixels N] [--allow name,...]
//                                  [--keep <dir>]
//   compare-shots <before> <after> --list-changed
//
// Every reported picture carries the box the differences fall inside, because a
// pixel count on its own is not a lead: `26-indexes` moved once by six and a half
// thousand pixels, the run said so and nothing else, and the pairs of runs after it
// did not reproduce it -- so there was nothing left to look at and the cause was
// never found. `--keep <dir>` goes further and copies both versions out, so a
// sighting survives the temporary directories the two runs went into.
//
// `--list-changed` prints one file name per line and nothing else, which is how
// `make guide-images` knows which pictures to copy over the committed ones: a
// picture that is only different by the renderer's own noise is left alone, so the
// commit holds the change and not fifty files of nothing.
//
// Exit 0 when nothing outside `--allow` changed, 1 when something did, 2 on a
// usage or read error.

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QSet>
#include <QStringList>
#include <QTextStream>

namespace {

struct Difference
{
    int changedPixels = 0;
    int worstDelta = 0;
    bool comparable = true;
    /// Where, in the picture's own coordinates. The count alone was not enough:
    /// `26-indexes` moved once by six and a half thousand pixels and the run said
    /// so and nothing else, so the next pair of runs -- which did not reproduce it
    /// -- had nothing to compare against and the cause was never found. A box says
    /// which part of the window to look at. See MOLE-261.
    QRect where;
};

Difference compare(const QImage& before, const QImage& after, int tolerance)
{
    Difference result;
    if (before.size() != after.size() || before.isNull() || after.isNull()) {
        result.comparable = false;
        return result;
    }

    const QImage a = before.convertToFormat(QImage::Format_RGB32);
    const QImage b = after.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < a.height(); ++y) {
        const auto* rowA = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const auto* rowB = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            const QRgb pa = rowA[x];
            const QRgb pb = rowB[x];
            if (pa == pb)
                continue;
            const int delta = std::max({ std::abs(qRed(pa) - qRed(pb)), std::abs(qGreen(pa) - qGreen(pb)),
                std::abs(qBlue(pa) - qBlue(pb)) });
            result.worstDelta = std::max(result.worstDelta, delta);
            if (delta > tolerance)
                ++result.changedPixels;
        }
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    QStringList arguments = QCoreApplication::arguments();
    arguments.removeFirst();

    int tolerance = 8;
    int pixelBudget = 0;
    bool listChanged = false;
    QString keepDirectory;
    QSet<QString> allowed;
    QStringList directories;
    for (int i = 0; i < arguments.size(); ++i) {
        const QString& argument = arguments.at(i);
        const auto value = [&]() { return i + 1 < arguments.size() ? arguments.at(++i) : QString(); };
        if (argument == QStringLiteral("--tolerance"))
            tolerance = value().toInt();
        else if (argument == QStringLiteral("--pixels"))
            pixelBudget = value().toInt();
        else if (argument == QStringLiteral("--list-changed"))
            listChanged = true;
        else if (argument == QStringLiteral("--keep"))
            keepDirectory = value();
        else if (argument == QStringLiteral("--allow")) {
            for (const QString& name : value().split(QLatin1Char(','), Qt::SkipEmptyParts))
                allowed.insert(name.trimmed());
        } else
            directories.append(argument);
    }

    if (directories.size() != 2) {
        err << "usage: compare-shots <before> <after> [--tolerance N] [--pixels N]"
               " [--allow name,...]\n";
        return 2;
    }

    const QDir before(directories.at(0));
    const QDir after(directories.at(1));
    // Named from `after`: it is the run that just happened, so a picture that is
    // new rather than changed is still one to copy. Reading the list from `before`
    // would silently skip every picture the guide does not have yet.
    const QStringList names = after.entryList({ QStringLiteral("*.png") }, QDir::Files, QDir::Name);
    if (names.isEmpty()) {
        err << "no pictures in " << after.path() << '\n';
        return 2;
    }

    // Copies of anything that moved, so a sighting outlives the run that saw it.
    if (!keepDirectory.isEmpty() && !QDir().mkpath(keepDirectory)) {
        err << "cannot write to " << keepDirectory << '\n';
        return 2;
    }
    const auto keep = [&](const QString& name, const QDir& from, const QString& suffix) {
        if (keepDirectory.isEmpty())
            return;
        const QString stem = QFileInfo(name).completeBaseName();
        QFile::remove(QDir(keepDirectory).filePath(stem + suffix));
        QFile::copy(from.filePath(name), QDir(keepDirectory).filePath(stem + suffix));
    };
    const auto describe = [](const Difference& difference) {
        return QStringLiteral("%1 pixels, worst %2, in %3x%4 at %5,%6")
            .arg(difference.changedPixels)
            .arg(difference.worstDelta)
            .arg(difference.where.width())
            .arg(difference.where.height())
            .arg(difference.where.left())
            .arg(difference.where.top());
    };

    int changed = 0;
    int excused = 0;
    int identical = 0;
    int noisy = 0;
    for (const QString& name : names) {
        const QImage a(before.filePath(name));
        const QImage b(after.filePath(name));
        const Difference difference = compare(a, b, tolerance);
        const QString stem = QFileInfo(name).completeBaseName();

        if (!difference.comparable) {
            // A missing or differently sized picture is a change nobody can excuse:
            // the tolerance is about rendering noise, not about a picture that is
            // not there.
            out << (listChanged ? name + QLatin1Char('\n') : QStringLiteral("  MISSING  %1\n").arg(name));
            ++changed;
            continue;
        }
        if (difference.changedPixels <= pixelBudget) {
            if (difference.worstDelta == 0)
                ++identical;
            else
                ++noisy;
            continue;
        }
        if (listChanged) {
            out << name << '\n';
            ++changed;
            continue;
        }
        if (allowed.contains(stem) || allowed.contains(name)) {
            out << QStringLiteral("  expected %1  (%2)\n").arg(name, -34).arg(describe(difference));
            ++excused;
            continue;
        }
        out << QStringLiteral("  CHANGED  %1  (%2)\n").arg(name, -34).arg(describe(difference));
        keep(name, before, QStringLiteral(".before.png"));
        keep(name, after, QStringLiteral(".after.png"));
        ++changed;
    }

    if (!listChanged) {
        out << QStringLiteral("\n%1 pictures: %2 byte-identical, %3 identical to the eye, "
                              "%4 expected to differ, %5 changed\n")
                   .arg(names.size())
                   .arg(identical)
                   .arg(noisy)
                   .arg(excused)
                   .arg(changed);
    }
    out.flush();
    return changed == 0 ? 0 : 1;
}
