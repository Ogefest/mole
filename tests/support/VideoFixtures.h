#pragma once

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

namespace mole::test::fixtures {

/// Which encoder to make a fixture with, out of what `ffmpeg -encoders` printed.
///
/// **An `ffmpeg` on the path is not an encoder.** This asked only whether the
/// binary existed and then asked it for libx264, which Fedora's ffmpeg-free leaves
/// out for patent reasons. So the cases stopped skipping the day the second-family
/// job installed ffmpeg, and failed on making their fixture. See MOLE-445.
///
/// libx264 first, because that is what every case here was written and checked
/// against. Then mpeg4 (MPEG-4 part 2), which is ffmpeg's own and is in every build
/// of it, free ones included, and which those same builds decode -- measured on
/// Fedora 45, where libopenh264 also works but comes from a separate repository.
/// Empty when neither is there, and the cases skip saying so.
inline QString chooseVideoEncoder(const QString& encoderListing)
{
    QStringList present;
    for (const QString& line : encoderListing.split(QLatin1Char('\n'))) {
        const QStringList columns = line.simplified().split(QLatin1Char(' '));
        // " V....D libx264  libx264 H.264 ..." -- the flags, then the name.
        if (columns.size() >= 2 && columns.at(0).startsWith(QLatin1Char('V')))
            present << columns.at(1);
    }
    for (const QString& wanted : { QStringLiteral("libx264"), QStringLiteral("mpeg4") }) {
        if (present.contains(wanted))
            return wanted;
    }
    return {};
}

/// The encoder this machine's ffmpeg will make fixtures with, asked once.
inline QString videoEncoder()
{
    static const QString chosen = [] {
        const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty())
            return QString();
        QProcess listing;
        listing.start(ffmpeg, { QStringLiteral("-hide_banner"), QStringLiteral("-encoders") });
        if (!listing.waitForStarted(5000) || !listing.waitForFinished(30000))
            return QString();
        return chooseVideoEncoder(QString::fromUtf8(listing.readAllStandardOutput()));
    }();
    return chosen;
}

/// Whether a video fixture can be made on this machine.
///
/// A committed video would be a binary blob nobody can review, and encoding one in
/// process would mean driving a recorder to test a decoder. So the fixture is made
/// by the encoder that is already installed beside the decoder Qt uses, and a
/// machine without one skips the tests that need it rather than pretending.
inline bool videoEncoderAvailable()
{
    return !videoEncoder().isEmpty();
}

/// A video whose **opening is deliberately black** and whose remainder is a bright
/// colour, at `path`: `blackSeconds` of black then `colourSeconds` of magenta.
///
/// That shape is the whole point: a great many videos open on black or on a fade
/// from it, so a thumbnailer that takes frame zero produces a folder of black
/// tiles. A fixture that opens on the colour could not tell the two apart.
///
/// The **length** matters as much as the opening, which is why it is a parameter.
/// A thumbnailer seeks a tenth of the way in, and one that fails to seek and plays
/// the opening instead still reaches the target of a short file inside its own time
/// limit -- so a short fixture cannot tell seeking from playing. See
/// writeMinuteLongVideoWithBlackOpening.
inline bool writeVideoOfLength(const QString& path, int blackSeconds, int colourSeconds)
{
    const QString encoder = videoEncoder();
    if (encoder.isEmpty())
        return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::remove(path);

    // Black then magenta, concatenated by the filter graph so there is one stream
    // and one timeline. Tiny frames and a low rate: what is being tested is which
    // frame, not how it looks, and a minute of flat colour at 160x120 and 10 fps is
    // a handful of kilobytes.
    QStringList arguments {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"),
        QStringLiteral("error"),
        QStringLiteral("-f"),
        QStringLiteral("lavfi"),
        QStringLiteral("-i"),
        QStringLiteral("color=c=black:s=160x120:r=10:d=%1").arg(blackSeconds),
        QStringLiteral("-f"),
        QStringLiteral("lavfi"),
        QStringLiteral("-i"),
        QStringLiteral("color=c=magenta:s=160x120:r=10:d=%1").arg(colourSeconds),
        QStringLiteral("-filter_complex"),
        QStringLiteral("[0:v][1:v]concat=n=2:v=1:a=0[out]"),
        QStringLiteral("-map"),
        QStringLiteral("[out]"),
        QStringLiteral("-c:v"),
        encoder,
        QStringLiteral("-pix_fmt"),
        QStringLiteral("yuv420p"),
    };
    if (encoder == QLatin1String("libx264"))
        arguments << QStringLiteral("-preset") << QStringLiteral("ultrafast");
    arguments << path;

    QProcess ffmpeg;
    ffmpeg.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), arguments);
    if (!ffmpeg.waitForStarted(5000))
        return false;
    if (!ffmpeg.waitForFinished(30000))
        return false;
    return ffmpeg.exitCode() == 0 && QFileInfo(path).size() > 0;
}

/// The five-second fixture the black-opening cases use: one second black, four
/// magenta. A thumbnailer aiming a tenth of the way in lands at its two-second
/// floor, which is inside the colour.
inline bool writeVideoWithBlackOpening(const QString& path)
{
    return writeVideoOfLength(path, 1, 4);
}

/// A **minute-long** fixture: three seconds black, fifty-seven magenta.
///
/// Long enough that the two behaviours come apart. A tenth of the way into sixty
/// seconds is six, and a thumbnailer that cannot seek and plays from the start
/// instead needs six seconds of wall clock to arrive there -- past the five-second
/// ceiling it gives itself, so it produces nothing at all. One that seeks answers
/// in well under a second. A five-second fixture cannot tell them apart, because
/// playing to its two-second target fits inside the same ceiling.
inline bool writeMinuteLongVideoWithBlackOpening(const QString& path)
{
    return writeVideoOfLength(path, 3, 57);
}

} // namespace mole::test::fixtures
