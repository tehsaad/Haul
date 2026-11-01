#include "MediaMerger.h"

#include "StreamDownloader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace haul {

namespace {

bool joinTrack(const StreamTrack &track, int trackIndex, const QString &directory, const QString &output,
               const std::atomic_bool &cancel, QString *error)
{
    QFile out(output);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("Cannot create the output file: %1").arg(out.errorString());
        return false;
    }
    QByteArray buffer;
    for (int i = 0; i < track.segments.size(); ++i) {
        QFile piece(streamPiecePath(directory, trackIndex, i));
        if (!piece.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Part of the video is missing.");
            return false;
        }
        while (!piece.atEnd()) {
            if (cancel.load()) {
                *error = QStringLiteral("Stopped by the user.");
                return false;
            }
            buffer = piece.read(1 << 20);
            if (out.write(buffer) != buffer.size()) {
                *error = QStringLiteral("Could not write to disk: %1").arg(out.errorString());
                return false;
            }
        }
    }
    return out.flush();
}

bool runFfmpeg(const QString &ffmpeg, const QStringList &arguments, const std::atomic_bool &cancel, QString *error)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    QStringList full { QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                       QStringLiteral("-protocol_whitelist"), QStringLiteral("file,pipe") };
    full += arguments;
    process.start(ffmpeg, full);
    if (!process.waitForStarted(10000)) {
        *error = QStringLiteral("FFmpeg could not be started.");
        return false;
    }
    while (!process.waitForFinished(200)) {
        if (cancel.load()) {
            process.kill();
            process.waitForFinished(3000);
            *error = QStringLiteral("Stopped by the user.");
            return false;
        }
        if (process.state() == QProcess::NotRunning)
            break;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QString::fromUtf8(process.readAll()).trimmed().left(300);
        return false;
    }
    return true;
}

QString containerFor(const QString &video, const QString &audio)
{
    if (video == QLatin1String("webm") && audio == QLatin1String("webm"))
        return QStringLiteral("webm");
    const QStringList mp4Family { QStringLiteral("mp4"), QStringLiteral("m4a"), QStringLiteral("ts"), QStringLiteral("aac") };
    if (mp4Family.contains(video) && mp4Family.contains(audio))
        return QStringLiteral("mp4");
    return QStringLiteral("mkv");
}

} // namespace

QString findFfmpeg()
{
    QString override = qEnvironmentVariable("HAUL_FFMPEG");
    if (!override.isEmpty())
        return override;
#if defined(Q_OS_WIN)
    const QString name = QStringLiteral("ffmpeg.exe");
#else
    const QString name = QStringLiteral("ffmpeg");
#endif
    const QString beside = QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;
    if (QFile::exists(beside))
        return beside;
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

MergeResult mergeStream(const QVector<StreamTrack> &tracks, const QString &directory, const std::atomic_bool &cancel,
                        const QString &ffmpeg)
{
    MergeResult result;
    QStringList joined;
    for (int t = 0; t < tracks.size(); ++t) {
        const QString path = QStringLiteral("%1/joined%2.%3").arg(directory).arg(t).arg(tracks[t].extension);
        if (!joinTrack(tracks[t], t, directory, path, cancel, &result.error))
            return result;
        joined.append(path);
    }

    if (tracks.size() == 1) {
        result.outputPath = joined.first();
        result.extension = tracks.first().extension;
        if (result.extension == QLatin1String("ts") && !ffmpeg.isEmpty()) {
            const QString mp4 = directory + QStringLiteral("/remuxed.mp4");
            QString failure;
            const QStringList input { QStringLiteral("-i"), joined.first(), QStringLiteral("-c"), QStringLiteral("copy") };
            // The filter is required for AAC in a transport stream but fails on other audio.
            if (runFfmpeg(ffmpeg, input + QStringList { QStringLiteral("-bsf:a"), QStringLiteral("aac_adtstoasc"), QStringLiteral("-y"), mp4 }, cancel, &failure)
                || (!cancel.load() && runFfmpeg(ffmpeg, input + QStringList { QStringLiteral("-y"), mp4 }, cancel, &failure))) {
                result.outputPath = mp4;
                result.extension = QStringLiteral("mp4");
            } else if (cancel.load()) {
                result.error = failure;
                return result;
            } else {
                result.note = QStringLiteral("Saved as .ts because it could not be converted to .mp4 (%1). It still plays in most players.").arg(failure);
            }
        }
        result.ok = true;
        return result;
    }

    const QString videoExtension = tracks[0].extension;
    const QString audioExtension = tracks[1].extension;
    if (ffmpeg.isEmpty()) {
        result.outputPath = joined[0];
        result.extension = videoExtension;
        result.extraPath = joined[1];
        result.extraExtension = audioExtension;
        result.note = QStringLiteral("This video has separate picture and sound. Install FFmpeg and Haul will join them; for now they were saved as two files.");
        result.ok = true;
        return result;
    }

    const QString container = containerFor(videoExtension, audioExtension);
    const QString merged = QStringLiteral("%1/merged.%2").arg(directory, container);
    QStringList arguments { QStringLiteral("-i"), joined[0], QStringLiteral("-i"), joined[1],
                            QStringLiteral("-map"), QStringLiteral("0:v:0"), QStringLiteral("-map"), QStringLiteral("1:a:0"),
                            QStringLiteral("-c"), QStringLiteral("copy") };
    if (audioExtension == QLatin1String("ts") || audioExtension == QLatin1String("aac"))
        arguments << QStringLiteral("-bsf:a") << QStringLiteral("aac_adtstoasc");
    arguments << QStringLiteral("-y") << merged;

    QString failure;
    if (!runFfmpeg(ffmpeg, arguments, cancel, &failure)) {
        if (cancel.load()) {
            result.error = failure;
            return result;
        }
        result.outputPath = joined[0];
        result.extension = videoExtension;
        result.extraPath = joined[1];
        result.extraExtension = audioExtension;
        result.note = QStringLiteral("FFmpeg could not join the picture and sound, so they were saved as two files.");
        result.ok = true;
        return result;
    }
    result.outputPath = merged;
    result.extension = container;
    result.ok = true;
    return result;
}

} // namespace haul
