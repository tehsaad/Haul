// Measures single-connection vs segmented downloads against a URL (normally the local test
// server). Usage: bench_segments <url> <size-bytes> <connections...>
// Prints wall-clock time and throughput for each connection count. Results are only as
// meaningful as the server: a server that limits speed per connection benefits from
// segmentation; a server or link that is already saturated does not.

#include "core/SegmentPlan.h"
#include "network/HttpDownloader.h"
#include "network/SegmentDownloader.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

using namespace haul;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <url> <size> <connections...>\n", argv[0]);
        return 2;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    const QUrl url(QString::fromLocal8Bit(argv[1]));
    const qint64 size = QByteArray(argv[2]).toLongLong();

    for (int i = 3; i < argc; ++i) {
        const int connections = QByteArray(argv[i]).toInt();
        QTemporaryDir dir;
        std::atomic_bool cancel { false };
        QElapsedTimer timer;
        timer.start();
        bool ok = false;
        if (connections <= 1) {
            SingleRequest r;
            r.url = url;
            r.filePath = dir.filePath("out");
            ok = !downloadSingle(r, cancel, nullptr).error.isError();
        } else {
            SegmentedRequest r;
            r.url = url;
            r.totalBytes = size;
            r.segments = planSegments(size, connections);
            r.segmentPath = [&](int n) { return dir.filePath(QString("seg%1").arg(n)); };
            QString error;
            ok = !downloadSegments(r, cancel, nullptr).error.isError()
                && assembleSegments(r.segments, r.segmentPath, dir.filePath("out"), size, cancel, &error);
        }
        const double seconds = timer.elapsed() / 1000.0;
        std::printf("connections=%d ok=%s time=%.2fs throughput=%.2f MiB/s\n", connections, ok ? "yes" : "NO",
                    seconds, size / 1048576.0 / seconds);
    }
    curl_global_cleanup();
    return 0;
}
