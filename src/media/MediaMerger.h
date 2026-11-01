#pragma once

#include "StreamTypes.h"

#include <atomic>

namespace haul {

// FFmpeg is optional and run as a separate program, which keeps Haul small and leaves its
// licence untouched. It is searched next to Haul, then on PATH. Empty if not found.
QString findFfmpeg();

struct MergeResult {
    bool ok = false;
    QString error;
    QString outputPath;  // the finished video, inside the stream folder
    QString extension;   // of outputPath, without the dot
    QString extraPath;   // audio that could not be joined without FFmpeg
    QString extraExtension;
    QString note;        // something the user should know
};

// Joins the pieces of each track in order, then:
//  * one track: keeps the file; a .ts is converted to .mp4 when FFmpeg is available.
//  * audio + video: merges them with FFmpeg, or leaves two files if FFmpeg is missing.
// FFmpeg is only ever given local files, so it can never fetch a remote stream or a key by itself.
MergeResult mergeStream(const QVector<StreamTrack> &tracks, const QString &directory, const std::atomic_bool &cancel,
                        const QString &ffmpeg);

} // namespace haul
