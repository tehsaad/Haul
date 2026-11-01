#include "MediaSniffer.h"

#include <cctype>

namespace haul {

namespace {

SniffResult result(SniffResult::Kind kind, const char *format)
{
    SniffResult r;
    r.kind = kind;
    r.format = QString::fromLatin1(format);
    return r;
}

unsigned char at(const QByteArray &data, int index)
{
    return static_cast<unsigned char>(data.at(index));
}

} // namespace

SniffResult sniffMedia(const QByteArray &head)
{
    if (head.size() >= 12 && head.mid(4, 4) == "ftyp") {
        const QByteArray brand = head.mid(8, 4);
        if (brand.startsWith("3gp") || brand.startsWith("3g2"))
            return result(SniffResult::Video, "3gp");
        if (brand == "M4A " || brand == "M4B " || brand == "M4P ")
            return result(SniffResult::Audio, "m4a");
        if (brand == "qt  ")
            return result(SniffResult::Video, "mov");
        if (brand.startsWith("M4V"))
            return result(SniffResult::Video, "m4v");
        return result(SniffResult::Video, "mp4");
    }
    if (head.size() >= 8 && (head.mid(4, 4) == "moov" || head.mid(4, 4) == "mdat" || head.mid(4, 4) == "wide"))
        return result(SniffResult::Video, "mov");
    if (head.startsWith("\x1A\x45\xDF\xA3"))
        return head.contains("webm") ? result(SniffResult::Video, "webm") : result(SniffResult::Video, "mkv");
    if (head.startsWith("OggS"))
        return head.contains("OpusHead") ? result(SniffResult::Audio, "opus")
            : head.contains("theora") ? result(SniffResult::Video, "ogv")
                                      : result(SniffResult::Audio, "ogg");
    if (head.startsWith("fLaC"))
        return result(SniffResult::Audio, "flac");
    if (head.startsWith("RIFF") && head.size() >= 12) {
        if (head.mid(8, 4) == "WAVE")
            return result(SniffResult::Audio, "wav");
        if (head.mid(8, 3) == "AVI")
            return result(SniffResult::Video, "avi");
    }
    if (head.startsWith("FLV\x01"))
        return result(SniffResult::Video, "flv");
    if (head.startsWith("ID3"))
        return result(SniffResult::Audio, "mp3");
    if (head.startsWith(QByteArray::fromHex("3026B2758E66CF11")))
        return result(SniffResult::Audio, "wma"); // the ASF container; the headers tell WMA from WMV
    if (head.startsWith(QByteArray::fromHex("000001BA")) || head.startsWith(QByteArray::fromHex("000001B3")))
        return result(SniffResult::Video, "mpeg");
    if (head.size() > 188 && at(head, 0) == 0x47 && at(head, 188) == 0x47)
        return result(SniffResult::Video, "ts");
    if (head.size() >= 2 && at(head, 0) == 0xFF && (at(head, 1) & 0xE0) == 0xE0) {
        const int layer = (at(head, 1) >> 1) & 0x03;
        if (layer == 0)
            return result(SniffResult::Audio, "aac"); // ADTS
        return result(SniffResult::Audio, "mp3");
    }

    // Not a media signature: look at the start of the text, ignoring a BOM and white space.
    QByteArray text = head.left(512);
    if (text.startsWith("\xEF\xBB\xBF"))
        text = text.mid(3);
    text = text.trimmed().toLower();
    if (text.startsWith("#extm3u") || text.contains("<mpd"))
        return result(SniffResult::Manifest, "");
    if (text.startsWith("<!doctype") || text.startsWith("<html") || text.startsWith("<?xml") || text.startsWith("<head")
        || text.startsWith("{") || text.startsWith("["))
        return result(SniffResult::NotMedia, "");
    return SniffResult();
}

} // namespace haul
