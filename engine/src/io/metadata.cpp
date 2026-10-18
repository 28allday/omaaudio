#include "metadata.h"

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <cstdio>
#include <ctime>
#include <memory>
#include <limits>
#include <set>
#include <vector>

namespace omawave::io {

namespace {

// Fixed-width, space- or NUL-padded chunk fields come back trimmed.
std::string field(const char* data, std::size_t width)
{
    std::size_t length = 0;
    while (length < width && data[length] != '\0') ++length;
    std::string out(data, length);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) out.pop_back();
    return out;
}

void put(char* data, std::size_t width, const std::string& value)
{
    std::memset(data, 0, width);
    std::memcpy(data, value.data(), std::min(width, value.size()));
}

std::string hex(const char* bytes, std::size_t count)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    bool any = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto b = static_cast<unsigned char>(bytes[i]);
        if (b != 0) any = true;
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return any ? out : std::string();
}

void unhex(const std::string& text, char* bytes, std::size_t count)
{
    std::memset(bytes, 0, count);
    for (std::size_t i = 0; i + 1 < text.size() && i / 2 < count; i += 2) {
        const auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        bytes[i / 2] = static_cast<char>((nibble(text[i]) << 4) | nibble(text[i + 1]));
    }
}

// The bext loudness fields are int16 hundredths; 0x7fff means "not set".
std::optional<double> loudnessField(std::int16_t value)
{
    if (value == 0x7fff) return std::nullopt;
    return value / 100.0;
}

std::int16_t loudnessField(std::optional<double> value)
{
    if (!value) return 0x7fff;
    return static_cast<std::int16_t>(std::clamp(*value * 100.0, -32768.0, 32767.0) + (*value >= 0 ? 0.5 : -0.5));
}

constexpr int kStringKeys[] = {SF_STR_TITLE, SF_STR_COPYRIGHT, SF_STR_SOFTWARE, SF_STR_ARTIST,
                               SF_STR_COMMENT, SF_STR_DATE, SF_STR_ALBUM, SF_STR_LICENSE,
                               SF_STR_TRACKNUMBER, SF_STR_GENRE};
const char* stringName(int key)
{
    switch (key) {
    case SF_STR_TITLE: return "title";
    case SF_STR_COPYRIGHT: return "copyright";
    case SF_STR_SOFTWARE: return "software";
    case SF_STR_ARTIST: return "artist";
    case SF_STR_COMMENT: return "comment";
    case SF_STR_DATE: return "date";
    case SF_STR_ALBUM: return "album";
    case SF_STR_LICENSE: return "license";
    case SF_STR_TRACKNUMBER: return "tracknumber";
    case SF_STR_GENRE: return "genre";
    }
    return "";
}

// A bext with room for a long coding history.
using BroadcastChunk = SF_BROADCAST_INFO_VAR(4096);

} // namespace

session::AudioMetadata readMetadata(SNDFILE* file)
{
    session::AudioMetadata out;
    if (file == nullptr) return out;

    auto chunk = std::make_unique<BroadcastChunk>();
    std::memset(chunk.get(), 0, sizeof(BroadcastChunk));
    if (sf_command(file, SFC_GET_BROADCAST_INFO, chunk.get(), sizeof(BroadcastChunk)) == SF_TRUE) {
        session::BroadcastInfo& b = out.bext;
        b.present = true;
        b.description = field(chunk->description, sizeof chunk->description);
        b.originator = field(chunk->originator, sizeof chunk->originator);
        b.originatorReference = field(chunk->originator_reference, sizeof chunk->originator_reference);
        b.originationDate = field(chunk->origination_date, sizeof chunk->origination_date);
        b.originationTime = field(chunk->origination_time, sizeof chunk->origination_time);
        b.timeReference = static_cast<std::int64_t>((static_cast<std::uint64_t>(chunk->time_reference_high) << 32)
                                                    | chunk->time_reference_low);
        b.version = chunk->version;
        b.umidHex = hex(chunk->umid, sizeof chunk->umid);
        const std::size_t history = std::min<std::size_t>(chunk->coding_history_size, sizeof chunk->coding_history);
        b.codingHistory = field(chunk->coding_history, history);
        if (chunk->version >= 2) {
            b.loudnessValue = loudnessField(chunk->loudness_value);
            b.loudnessRange = loudnessField(chunk->loudness_range);
            b.maxTruePeak = loudnessField(chunk->max_true_peak_level);
            b.maxMomentaryLoudness = loudnessField(chunk->max_momentary_loudness);
            b.maxShortTermLoudness = loudnessField(chunk->max_shortterm_loudness);
        }
    }

    SF_CHUNK_INFO wanted{};
    std::snprintf(wanted.id, sizeof wanted.id, "iXML");
    wanted.id_size = 4;
    if (SF_CHUNK_ITERATOR* it = sf_get_chunk_iterator(file, &wanted); it != nullptr) {
        SF_CHUNK_INFO info{};
        if (sf_get_chunk_size(it, &info) == SF_ERR_NO_ERROR && info.datalen > 0 && info.datalen < (16u << 20)) {
            std::vector<char> data(info.datalen);
            info.data = data.data();
            if (sf_get_chunk_data(it, &info) == SF_ERR_NO_ERROR) {
                std::size_t length = info.datalen;
                while (length > 0 && data[length - 1] == '\0') --length;
                out.ixml.assign(data.data(), length);
            }
        }
    }

    for (const int key : kStringKeys) {
        if (const char* value = sf_get_string(file, key); value != nullptr && *value != '\0')
            out.strings[stringName(key)] = value;
    }
    return out;
}

session::AudioMetadata readMetadata(SNDFILE* file, const std::string& path)
{
    session::AudioMetadata out = readMetadata(file);
    if (!path.empty()) out.markers = readMarkerChunks(path);
    return out;
}

session::AudioMetadata readMetadata(const std::string& path)
{
    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    if (file == nullptr) return {};
    session::AudioMetadata out = readMetadata(file, path);
    sf_close(file);
    return out;
}

namespace {

std::uint32_t le32(const unsigned char* p) noexcept
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::uint64_t le64(const unsigned char* p) noexcept { return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32); }
void putLe32(std::string& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

struct FileCloser {
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
};

struct RiffFile {
    bool ok = false, rf64 = false;
    std::unique_ptr<std::FILE, FileCloser> file;
    std::uint64_t size = 0;
    std::uint64_t riffSize = 0;     // the size the header claims, from RIFF or ds64
    std::uint64_t dataSize = 0;     // RF64: the data chunk's true size, from ds64
};

bool readAt(const RiffFile& riff, std::uint64_t at, void* data, std::size_t size)
{
    return at <= riff.size && size <= riff.size - at
        && ::fseeko(riff.file.get(), static_cast<off_t>(at), SEEK_SET) == 0
        && std::fread(data, 1, size, riff.file.get()) == size;
}

// Audio is skipped with seeks. Only headers and bounded marker payloads enter
// RAM, including when the audio chunk occupies several gigabytes.
constexpr std::size_t kMaxMarkerBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxMarkerIds = 65536;

RiffFile readRiff(const std::string& path, const char* mode = "rb")
{
    RiffFile r;
    r.file.reset(std::fopen(path.c_str(), mode));
    if (!r.file || ::fseeko(r.file.get(), 0, SEEK_END) != 0) return r;
    const auto size = ::ftello(r.file.get());
    if (size < 12) return r;
    r.size = static_cast<std::uint64_t>(size);
    std::array<unsigned char, 12> header{};
    if (!readAt(r, 0, header.data(), header.size()) || std::memcmp(header.data() + 8, "WAVE", 4) != 0) return r;
    if (std::memcmp(header.data(), "RIFF", 4) == 0) r.riffSize = le32(header.data() + 4);
    else if (std::memcmp(header.data(), "RF64", 4) == 0) {
        std::array<unsigned char, 36> ds64{};
        if (!readAt(r, 12, ds64.data(), ds64.size()) || std::memcmp(ds64.data(), "ds64", 4) != 0
            || le32(ds64.data() + 4) < 28 || le32(ds64.data() + 4) > r.size - 20) return r;
        r.rf64 = true;
        r.riffSize = le64(ds64.data() + 8);
        r.dataSize = le64(ds64.data() + 16);
    } else return r;
    r.ok = r.riffSize >= 4 && r.riffSize <= r.size - 8;
    return r;
}

struct RiffChunk {
    std::string id;
    std::uint64_t data = 0, size = 0, next = 0;
};

bool readChunk(const RiffFile& riff, std::uint64_t at, std::uint64_t end, RiffChunk& chunk)
{
    if (at > end || end - at < 8) return false;
    std::array<unsigned char, 8> header{};
    if (!readAt(riff, at, header.data(), header.size())) return false;
    chunk.id.assign(reinterpret_cast<const char*>(header.data()), 4);
    chunk.data = at + 8;
    chunk.size = le32(header.data() + 4);
    if (riff.rf64 && chunk.id == "data" && chunk.size == 0xffffffffu) chunk.size = riff.dataSize;
    if (chunk.size > end - chunk.data) return false;
    chunk.next = chunk.data + chunk.size;
    if (chunk.size & 1) {
        if (chunk.next == end) return false;
        ++chunk.next;
    }
    return true;
}

} // namespace

std::vector<session::FileMarker> readMarkerChunks(const std::string& path)
{
    std::vector<session::FileMarker> markers;
    const RiffFile r = readRiff(path);
    if (!r.ok) return markers;
    std::map<std::uint32_t, session::FileMarker> byId;
    std::vector<std::uint32_t> order;
    std::set<std::uint32_t> seenCues;
    std::size_t remaining = kMaxMarkerBytes;
    const auto end = r.riffSize + 8;
    for (std::uint64_t at = 12; at < end;) {
        RiffChunk chunk;
        if (!readChunk(r, at, end, chunk)) break;
        at = chunk.next;
        const auto& id = chunk.id;
        if (id != "cue " && id != "LIST") continue;
        if (chunk.size < 4) continue;
        if (id == "LIST") {
            std::array<unsigned char, 4> kind{};
            if (chunk.size < 4 || !readAt(r, chunk.data, kind.data(), kind.size())
                || std::memcmp(kind.data(), "adtl", 4) != 0) continue;
        }
        if (chunk.size > remaining) return {};
        const auto size = static_cast<std::size_t>(chunk.size);
        remaining -= size;
        std::vector<unsigned char> payload(size);
        if (!readAt(r, chunk.data, payload.data(), size)) return {};
        const auto* b = payload.data();
        if (id == "cue " && size >= 4) {
            const auto count = std::min<std::size_t>(le32(b), (size - 4) / 24);
            for (std::size_t i = 0; i < count; ++i) {
                const unsigned char* c = b + 4 + i * 24;
                const std::uint32_t cueId = le32(c);
                if (!byId.contains(cueId) && byId.size() == kMaxMarkerIds) return {};
                session::FileMarker& m = byId[cueId];
                m.position = le32(c + 20);
                if (seenCues.insert(cueId).second) order.push_back(cueId);
            }
        } else if (id == "LIST") {
            std::size_t sub = 4;
            while (sub <= size && size - sub >= 8) {
                const std::string subId(reinterpret_cast<const char*>(b + sub), 4);
                const std::uint32_t subSize = le32(b + sub + 4);
                const std::size_t body = sub + 8;
                if (subSize > size - body) break;
                if ((subId == "labl" || subId == "ltxt") && subSize >= 4
                    && !byId.contains(le32(b + body)) && byId.size() == kMaxMarkerIds) return {};
                if (subId == "labl" && subSize >= 4) {
                    const std::uint32_t cueId = le32(b + body);
                    byId[cueId].label = field(reinterpret_cast<const char*>(b + body + 4), subSize - 4);
                } else if (subId == "ltxt" && subSize >= 8) {
                    byId[le32(b + body)].length = le32(b + body + 4);
                }
                sub = body + subSize + (subSize & 1);
            }
        }
    }
    for (const std::uint32_t id : order) markers.push_back(byId[id]);
    std::stable_sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) { return a.position < b.position; });
    return markers;
}

std::string appendMarkers(const std::string& path, const std::vector<session::FileMarker>& markers)
{
    if (markers.empty()) return {};
    if (markers.size() > kMaxMarkerIds) return "too many file markers (maximum 65536)";
    std::size_t payloadBytes = 8 + markers.size() * 24;
    for (const auto& marker : markers) {
        if (marker.label.size() >= kMaxMarkerBytes) return "file marker metadata exceeds 16 MiB";
        payloadBytes += 12 + ((marker.label.size() + 2) & ~std::size_t{1}) + (marker.length > 0 ? 28 : 0);
        if (payloadBytes > kMaxMarkerBytes) return "file marker metadata exceeds 16 MiB";
    }
    RiffFile r = readRiff(path, "r+b");
    if (!r.ok) return "markers can be appended to WAV and RF64 files only";
    std::string cue, adtl = "adtl";
    putLe32(cue, static_cast<std::uint32_t>(markers.size()));
    for (std::size_t i = 0; i < markers.size(); ++i) {
        const session::FileMarker& m = markers[i];
        const auto id = static_cast<std::uint32_t>(i + 1);
        const auto position = static_cast<std::uint32_t>(std::clamp<std::int64_t>(m.position, 0, 0xffffffffLL));
        putLe32(cue, id); putLe32(cue, position); cue += "data"; putLe32(cue, 0); putLe32(cue, 0); putLe32(cue, position);
        std::string text = m.label; text.push_back('\0'); if (text.size() & 1) text.push_back('\0');
        adtl += "labl"; putLe32(adtl, static_cast<std::uint32_t>(4 + text.size())); putLe32(adtl, id); adtl += text;
        if (m.length > 0) {
            // ltxt: cue id, length, purpose "rgn ", country, language, dialect, code page, no text.
            adtl += "ltxt"; putLe32(adtl, 20); putLe32(adtl, id);
            putLe32(adtl, static_cast<std::uint32_t>(std::clamp<std::int64_t>(m.length, 0, 0xffffffffLL)));
            adtl += "rgn "; putLe32(adtl, 0); putLe32(adtl, 0);
        }
    }
    std::string chunks = "cue ";
    putLe32(chunks, static_cast<std::uint32_t>(cue.size())); chunks += cue;
    chunks += "LIST"; putLe32(chunks, static_cast<std::uint32_t>(adtl.size())); chunks += adtl;
    if (adtl.size() & 1) chunks.push_back('\0');

    if (chunks.size() > std::uint64_t(std::numeric_limits<off_t>::max()) - r.size)
        return "marker chunks exceed the file size limit";
    const std::uint64_t total = r.size + chunks.size() - 8;
    if (!r.rf64 && total > 0xffffffffu) return "marker chunks need an RF64 file";
    auto* f = r.file.get();
    if (::fseeko(f, 0, SEEK_END) != 0) return "could not seek to append marker chunks";
    const bool wrote = std::fwrite(chunks.data(), 1, chunks.size(), f) == chunks.size();
    std::string size;
    if (r.rf64) {
        for (int i = 0; i < 8; ++i) size.push_back(static_cast<char>((total >> (8 * i)) & 0xff));
        if (::fseeko(f, 20, SEEK_SET) != 0) return "could not seek to update the RF64 header";
    } else {
        putLe32(size, static_cast<std::uint32_t>(total));
        if (::fseeko(f, 4, SEEK_SET) != 0) return "could not seek to update the WAV header";
    }
    const bool patched = std::fwrite(size.data(), 1, size.size(), f) == size.size();
    const bool closed = std::fclose(r.file.release()) == 0;
    return wrote && patched && closed ? std::string() : "could not append the marker chunks";
}

std::string writeMetadata(SNDFILE* file, const session::AudioMetadata& metadata)
{
    if (file == nullptr) return "no file";
    if (metadata.bext.present) {
        const session::BroadcastInfo& b = metadata.bext;
        auto chunk = std::make_unique<BroadcastChunk>();
        std::memset(chunk.get(), 0, sizeof(BroadcastChunk));
        put(chunk->description, sizeof chunk->description, b.description);
        put(chunk->originator, sizeof chunk->originator, b.originator);
        put(chunk->originator_reference, sizeof chunk->originator_reference, b.originatorReference);
        put(chunk->origination_date, sizeof chunk->origination_date, b.originationDate);
        put(chunk->origination_time, sizeof chunk->origination_time, b.originationTime);
        const auto reference = static_cast<std::uint64_t>(std::max<std::int64_t>(0, b.timeReference));
        chunk->time_reference_low = static_cast<std::uint32_t>(reference & 0xffffffffu);
        chunk->time_reference_high = static_cast<std::uint32_t>(reference >> 32);
        const bool loudness = b.loudnessValue || b.loudnessRange || b.maxTruePeak
                           || b.maxMomentaryLoudness || b.maxShortTermLoudness;
        chunk->version = static_cast<short>(loudness ? 2 : (!b.umidHex.empty() ? 1 : std::max(b.version, 0)));
        if (chunk->version > 2) chunk->version = 2;
        unhex(b.umidHex, chunk->umid, sizeof chunk->umid);
        chunk->loudness_value = loudnessField(b.loudnessValue);
        chunk->loudness_range = loudnessField(b.loudnessRange);
        chunk->max_true_peak_level = loudnessField(b.maxTruePeak);
        chunk->max_momentary_loudness = loudnessField(b.maxMomentaryLoudness);
        chunk->max_shortterm_loudness = loudnessField(b.maxShortTermLoudness);
        const std::string history = b.codingHistory.substr(0, sizeof chunk->coding_history - 2);
        std::memcpy(chunk->coding_history, history.data(), history.size());
        chunk->coding_history_size = static_cast<std::uint32_t>(history.size());
        if (sf_command(file, SFC_SET_BROADCAST_INFO, chunk.get(), sizeof(BroadcastChunk)) != SF_TRUE)
            return std::string("the broadcast information could not be written: ") + sf_strerror(file);
    }
    if (!metadata.ixml.empty()) {
        std::string data = metadata.ixml;
        if (data.size() % 2) data.push_back('\0');
        SF_CHUNK_INFO info{};
        std::snprintf(info.id, sizeof info.id, "iXML");
        info.id_size = 4;
        info.datalen = static_cast<unsigned>(data.size());
        info.data = data.data();
        if (sf_set_chunk(file, &info) != SF_ERR_NO_ERROR)
            return "the iXML chunk could not be written";
    }
    for (const auto& [name, value] : metadata.strings) {
        for (const int key : kStringKeys) {
            if (name == stringName(key) && !value.empty()) sf_set_string(file, key, value.c_str());
        }
    }
    return {};
}

std::string todayForBext()
{
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buffer[16];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%d", &local);
    return buffer;
}

std::string nowForBext()
{
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buffer[16];
    std::strftime(buffer, sizeof buffer, "%H:%M:%S", &local);
    return buffer;
}

std::string codingHistoryLine(std::uint32_t rate, int bits, std::uint32_t channels, const std::string& tool)
{
    return "A=PCM,F=" + std::to_string(rate) + ",W=" + std::to_string(bits) + ",M="
         + (channels == 1 ? "mono" : channels == 2 ? "stereo" : std::to_string(channels) + "ch")
         + ",T=" + tool + "\r\n";
}

} // namespace omawave::io
