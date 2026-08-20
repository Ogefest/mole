#pragma once

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QString>

namespace mole::test::fixtures {

/// Whether a video fixture can be made on this machine.
///
/// A committed video would be a binary blob nobody can review, and encoding one in
/// process would mean driving a recorder to test a decoder. So the fixture is made
/// by the encoder that is already installed beside the decoder Qt uses, and a
/// machine without it skips the tests that need one rather than pretending.
inline bool videoEncoderAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty();
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
    if (!videoEncoderAvailable())
        return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::remove(path);

    // Black then magenta, concatenated by the filter graph so there is one stream
    // and one timeline. Tiny frames and a low rate: what is being tested is which
    // frame, not how it looks, and a minute of flat colour at 160x120 and 10 fps is
    // a handful of kilobytes.
    const QStringList arguments {
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
        QStringLiteral("libx264"),
        QStringLiteral("-preset"),
        QStringLiteral("ultrafast"),
        QStringLiteral("-pix_fmt"),
        QStringLiteral("yuv420p"),
        path,
    };

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
