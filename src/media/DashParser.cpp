#include "DashParser.h"

#include <QRegularExpression>
#include <QXmlStreamReader>

#include <cmath>
#include <memory>
#include <vector>

namespace haul {

namespace {

constexpr int kMaxDepth = 32;
constexpr int kMaxNodes = 2000000;

// A small tree of the manifest; DASH settings inherit downward, which is easiest on a tree.
struct Node {
    QString name;
    QXmlStreamAttributes attributes;
    QString text;
    std::vector<std::unique_ptr<Node>> children;

    QString attr(const char *key) const { return attributes.value(QLatin1String(key)).toString(); }
    bool has(const char *key) const { return attributes.hasAttribute(QLatin1String(key)); }
    const Node *child(const char *childName) const
    {
        for (const auto &c : children) {
            if (c->name == QLatin1String(childName))
                return c.get();
        }
        return nullptr;
    }
    bool hasChild(const char *childName) const { return child(childName) != nullptr; }
};

std::unique_ptr<Node> readTree(const QByteArray &body, QString *error)
{
    QXmlStreamReader reader(body); // never fetches external entities or DTDs
    auto root = std::make_unique<Node>();
    std::vector<Node *> stack { root.get() };
    int nodes = 0;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (++nodes > kMaxNodes || static_cast<int>(stack.size()) > kMaxDepth) {
                *error = QStringLiteral("The manifest is too large.");
                return nullptr;
            }
            auto node = std::make_unique<Node>();
            node->name = reader.name().toString();
            node->attributes = reader.attributes();
            Node *raw = node.get();
            stack.back()->children.push_back(std::move(node));
            stack.push_back(raw);
        } else if (reader.isEndElement()) {
            stack.pop_back();
        } else if (reader.isCharacters()) {
            stack.back()->text += reader.text();
        }
    }
    if (reader.hasError()) {
        *error = QStringLiteral("The manifest is not valid: %1").arg(reader.errorString());
        return nullptr;
    }
    return root;
}

QUrl withBaseUrl(const QUrl &current, const Node &node)
{
    if (const Node *base = node.child("BaseURL"))
        return current.resolved(QUrl(base->text.trimmed()));
    return current;
}

struct TimelineEntry {
    qint64 time;
    qint64 duration;
    qint64 repeat;
};

struct Template {
    QString media;
    QString initialization;
    qint64 startNumber = 1;
    qint64 timescale = 1;
    qint64 duration = 0;
    QVector<TimelineEntry> timeline;
    bool present = false;

    void inherit(const Node &parent)
    {
        const Node *node = parent.child("SegmentTemplate");
        if (!node)
            return;
        present = true;
        if (node->has("media"))
            media = node->attr("media");
        if (node->has("initialization"))
            initialization = node->attr("initialization");
        if (node->has("startNumber"))
            startNumber = node->attr("startNumber").toLongLong();
        if (node->has("timescale"))
            timescale = std::max<qint64>(node->attr("timescale").toLongLong(), 1);
        if (node->has("duration"))
            duration = node->attr("duration").toLongLong();
        if (const Node *line = node->child("SegmentTimeline")) {
            timeline.clear();
            for (const auto &s : line->children) {
                if (s->name != QLatin1String("S"))
                    continue;
                timeline.append({ s->has("t") ? s->attr("t").toLongLong() : -1, s->attr("d").toLongLong(),
                                  s->attr("r").toLongLong() });
            }
        }
    }
};

QString expandTemplate(QString pattern, const QString &id, qint64 bandwidth, qint64 number, qint64 time)
{
    static const QRegularExpression token(QStringLiteral(R"(\$(RepresentationID|Number|Time|Bandwidth|)(%0(\d+)d)?\$)"));
    QString out;
    int last = 0;
    auto it = token.globalMatch(pattern);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += pattern.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();
        const QString name = m.captured(1);
        const int width = m.captured(3).toInt();
        if (name.isEmpty())
            out += QLatin1Char('$');
        else if (name == QLatin1String("RepresentationID"))
            out += id;
        else {
            const qint64 value = name == QLatin1String("Number") ? number : name == QLatin1String("Time") ? time : bandwidth;
            out += QStringLiteral("%1").arg(value, width, 10, QLatin1Char('0'));
        }
    }
    out += pattern.mid(last);
    return out;
}

bool buildFromTemplate(const Template &t, const QUrl &base, double totalSeconds, DashRepresentation *rep, QString *error)
{
    if (t.media.isEmpty()) {
        *error = QStringLiteral("The manifest doesn't say where its pieces are.");
        return false;
    }
    if (!t.initialization.isEmpty()) {
        MediaSegment init;
        init.url = base.resolved(QUrl(expandTemplate(t.initialization, rep->id, rep->bandwidth, 0, 0))).toString(QUrl::FullyEncoded);
        init.isInit = true;
        rep->segments.append(init);
    }

    auto addSegment = [&](qint64 number, qint64 time, qint64 duration) {
        MediaSegment segment;
        segment.url = base.resolved(QUrl(expandTemplate(t.media, rep->id, rep->bandwidth, number, time))).toString(QUrl::FullyEncoded);
        segment.duration = static_cast<double>(duration) / t.timescale;
        rep->segments.append(segment);
        return rep->segments.size() <= kMaxStreamSegments;
    };

    if (!t.timeline.isEmpty()) {
        qint64 number = t.startNumber;
        qint64 time = 0;
        for (int i = 0; i < t.timeline.size(); ++i) {
            const TimelineEntry &entry = t.timeline[i];
            if (entry.time >= 0)
                time = entry.time;
            if (entry.duration <= 0) {
                *error = QStringLiteral("The manifest has an invalid timeline.");
                return false;
            }
            qint64 repeat = entry.repeat;
            if (repeat < 0) { // "until the next entry or the end of the video"
                const qint64 end = i + 1 < t.timeline.size() && t.timeline[i + 1].time >= 0
                    ? t.timeline[i + 1].time
                    : static_cast<qint64>(std::ceil(totalSeconds * t.timescale));
                repeat = std::max<qint64>((end - time + entry.duration - 1) / entry.duration - 1, 0);
            }
            for (qint64 r = 0; r <= repeat; ++r) {
                if (!addSegment(number++, time, entry.duration)) {
                    *error = QStringLiteral("The manifest has too many pieces.");
                    return false;
                }
                time += entry.duration;
            }
        }
        return true;
    }

    if (t.duration <= 0 || totalSeconds <= 0) {
        *error = QStringLiteral("The manifest doesn't say how long the video is.");
        return false;
    }
    const qint64 count = static_cast<qint64>(std::ceil(totalSeconds * t.timescale / t.duration));
    for (qint64 i = 0; i < count; ++i) {
        if (!addSegment(t.startNumber + i, i * t.duration, t.duration)) {
            *error = QStringLiteral("The manifest has too many pieces.");
            return false;
        }
    }
    return true;
}

bool parseRange(const QString &text, qint64 *start, qint64 *length)
{
    const QStringList parts = text.split(QLatin1Char('-'));
    if (parts.size() != 2)
        return false;
    *start = parts[0].toLongLong();
    *length = parts[1].toLongLong() - *start + 1;
    return *length > 0;
}

bool buildFromList(const Node &list, const QUrl &base, double totalSeconds, DashRepresentation *rep)
{
    if (const Node *init = list.child("Initialization")) {
        MediaSegment segment;
        segment.url = base.resolved(QUrl(init->attr("sourceURL"))).toString(QUrl::FullyEncoded);
        segment.isInit = true;
        qint64 start = 0;
        qint64 length = 0;
        if (parseRange(init->attr("range"), &start, &length)) {
            segment.rangeStart = start;
            segment.rangeLength = length;
        }
        rep->segments.append(segment);
    }
    QVector<MediaSegment> media;
    for (const auto &entry : list.children) {
        if (entry->name != QLatin1String("SegmentURL"))
            continue;
        MediaSegment segment;
        segment.url = base.resolved(QUrl(entry->attr("media"))).toString(QUrl::FullyEncoded);
        qint64 start = 0;
        qint64 length = 0;
        if (parseRange(entry->attr("mediaRange"), &start, &length)) {
            segment.rangeStart = start;
            segment.rangeLength = length;
        }
        media.append(segment);
    }
    const double scale = std::max(list.attr("timescale").toDouble(), 1.0);
    const double each = list.has("duration") ? list.attr("duration").toDouble() / scale
                                             : (media.isEmpty() ? 0.0 : totalSeconds / media.size());
    for (MediaSegment &segment : media)
        segment.duration = each;
    rep->segments += media;
    return !media.isEmpty();
}

bool hasProtection(const Node &node)
{
    return node.hasChild("ContentProtection");
}

QString extensionFor(const QString &mime, bool isVideo)
{
    if (mime.contains(QLatin1String("webm")))
        return QStringLiteral("webm");
    if (mime.contains(QLatin1String("mp2t")))
        return QStringLiteral("ts");
    return isVideo ? QStringLiteral("mp4") : QStringLiteral("m4a");
}

} // namespace

double parseIsoDuration(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^P(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+(?:\.\d+)?)S)?)?$)"));
    const QRegularExpressionMatch m = pattern.match(text.trimmed());
    if (!m.hasMatch() || text.trimmed().size() < 2)
        return -1.0;
    return m.captured(1).toDouble() * 86400.0 + m.captured(2).toDouble() * 3600.0
        + m.captured(3).toDouble() * 60.0 + m.captured(4).toDouble();
}

bool isDashManifest(const QByteArray &body)
{
    return body.left(4096).contains("<MPD");
}

bool parseDashManifest(const QByteArray &body, const QUrl &base, DashManifest *manifest, QString *error)
{
    const std::unique_ptr<Node> tree = readTree(body, error);
    if (!tree)
        return false;
    const Node *mpd = tree->child("MPD");
    if (!mpd) {
        *error = QStringLiteral("This is not a valid DASH manifest.");
        return false;
    }

    manifest->live = mpd->attr("type") == QLatin1String("dynamic");
    if (manifest->live)
        return true; // the caller reports this; there is nothing fixed to list

    QVector<const Node *> periods;
    for (const auto &c : mpd->children) {
        if (c->name == QLatin1String("Period"))
            periods.append(c.get());
    }
    manifest->periods = periods.size();
    if (periods.isEmpty()) {
        *error = QStringLiteral("The manifest doesn't list any video.");
        return false;
    }
    if (periods.size() > 1) {
        *error = QStringLiteral("This video is split into several periods, which Haul doesn't support yet.");
        return false;
    }

    const Node &period = *periods.first();
    manifest->duration = std::max(parseIsoDuration(mpd->attr("mediaPresentationDuration")), parseIsoDuration(period.attr("duration")));
    const QUrl periodBase = withBaseUrl(withBaseUrl(base, *mpd), period);

    for (const auto &set : period.children) {
        if (set->name != QLatin1String("AdaptationSet"))
            continue;
        const QUrl setBase = withBaseUrl(periodBase, *set);
        Template setTemplate;
        setTemplate.inherit(period);
        setTemplate.inherit(*set);

        for (const auto &repNode : set->children) {
            if (repNode->name != QLatin1String("Representation"))
                continue;
            const QString mime = repNode->has("mimeType") ? repNode->attr("mimeType") : set->attr("mimeType");
            const QString type = set->has("contentType") ? set->attr("contentType") : mime.section(QLatin1Char('/'), 0, 0);
            if (type != QLatin1String("video") && type != QLatin1String("audio"))
                continue; // subtitles and images

            DashRepresentation rep;
            rep.id = repNode->attr("id");
            rep.isVideo = type == QLatin1String("video");
            rep.encrypted = hasProtection(*set) || hasProtection(*repNode);
            rep.width = repNode->attr("width").toInt();
            rep.height = repNode->attr("height").toInt();
            rep.bandwidth = repNode->attr("bandwidth").toLongLong();
            rep.codecs = repNode->has("codecs") ? repNode->attr("codecs") : set->attr("codecs");
            rep.isMuxed = rep.isVideo && (rep.codecs.contains(QLatin1String("mp4a")) || rep.codecs.contains(QLatin1String("opus")) || rep.codecs.contains(QLatin1String("vorbis")));
            rep.extension = extensionFor(mime, rep.isVideo);

            const QUrl repBase = withBaseUrl(setBase, *repNode);
            Template repTemplate = setTemplate;
            repTemplate.inherit(*repNode);
            const Node *list = repNode->child("SegmentList");
            if (!list)
                list = set->child("SegmentList");

            if (repTemplate.present) {
                if (!buildFromTemplate(repTemplate, repBase, manifest->duration, &rep, error))
                    return false;
            } else if (list) {
                if (!buildFromList(*list, repBase, manifest->duration, &rep)) {
                    *error = QStringLiteral("The manifest doesn't say where its pieces are.");
                    return false;
                }
            } else if (repNode->hasChild("BaseURL") || set->hasChild("BaseURL")) {
                // SegmentBase, or just one file: fetch the whole file as a single piece.
                MediaSegment whole;
                whole.url = repBase.toString(QUrl::FullyEncoded);
                whole.duration = manifest->duration;
                rep.segments.append(whole);
            } else {
                *error = QStringLiteral("The manifest doesn't say where its pieces are.");
                return false;
            }
            manifest->representations.append(rep);
        }
    }

    if (manifest->representations.isEmpty()) {
        *error = QStringLiteral("The manifest doesn't list any video.");
        return false;
    }
    return true;
}

} // namespace haul
