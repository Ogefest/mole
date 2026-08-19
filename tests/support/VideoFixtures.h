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

/// A five-second video whose **first second is deliberately black** and whose
/// remainder is a bright colour, at `path`.
///
/// That shape is the whole point: a great many videos open on black or on a fade
/// from it, so a thumbnailer that takes frame zero produces a folder of black
/// tiles. A fixture that opens on the colour could not tell the two apart.
inline bool writeVideoWithBlackOpening(const QString& path)
{
    if (!videoEncoderAvailable())
        return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::remove(path);

    // One second of black then four of magenta, concatenated by the filter graph
    // so there is one stream and one timeline. Tiny frames and a low rate: what is
    // being tested is which frame, not how it looks.
    const QStringList arguments {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"),
        QStringLiteral("error"),
        QStringLiteral("-f"),
        QStringLiteral("lavfi"),
        QStringLiteral("-i"),
        QStringLiteral("color=c=black:s=160x120:r=10:d=1"),
        QStringLiteral("-f"),
        QStringLiteral("lavfi"),
        QStringLiteral("-i"),
        QStringLiteral("color=c=magenta:s=160x120:r=10:d=4"),
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

} // namespace mole::test::fixtures
