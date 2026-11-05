/*
 * Copyright (c) 2026 CasparVP contributors
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 */

#include "replay_store.h"

#include <common/env.h>
#include <common/log.h>
#include <common/utf.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)
#include <nmmintrin.h>
#define REPLAY_HAVE_SSE42_CRC 1
#endif

// MSVC emits any intrinsic regardless of /arch (and this tree builds with /arch:AVX2). GCC and
// clang refuse to inline one the translation unit's target does not enable, and the Linux build's
// baseline is SSE4.1 -- so the CRC function names SSE4.2 itself.
#if defined(REPLAY_HAVE_SSE42_CRC) && (defined(__GNUC__) || defined(__clang__))
#define REPLAY_TARGET_SSE42 __attribute__((target("sse4.2")))
#else
#define REPLAY_TARGET_SSE42
#endif

namespace caspar { namespace replay { namespace store {

namespace {

// ── On-disk layout ──────────────────────────────────────────────────────────────────────────
//
// Little-endian, packed, fixed sizes, every structure with its own length so a later version can
// grow one without breaking an older reader. Sizes are asserted below: they ARE the format.

constexpr char     kSegmentMagic[8] = {'C', 'V', 'P', 'R', 'P', 'L', '2', '\0'};
constexpr char     kIndexMagic[8]   = {'C', 'V', 'P', 'R', 'I', 'X', '2', '\0'};
constexpr uint32_t kRecordMagic     = 0x32435252; // "RRC2" read as little-endian bytes R R C 2
constexpr uint32_t kVersion         = 1;

#pragma pack(push, 1)
struct segment_header
{
    char     magic[8];
    uint32_t version;
    uint32_t header_bytes;
    uint32_t segment_index;
    uint32_t codec;
    int32_t  width, height, rate_num, rate_den, field_count, bit_depth;
    int32_t  color_space, color_transfer, audio_channels, sample_rate;
    uint32_t vmx_profile;
    uint64_t created_us;
    uint8_t  reserved[256 - 8 - 4 * 4 - 4 * 10 - 4 - 8 - 4];
    uint32_t header_crc; ///< CRC-32C of every byte before it
};
static_assert(sizeof(segment_header) == 256, "segment header is 256 bytes");

struct record_header
{
    uint32_t magic;
    uint32_t header_bytes;
    int64_t  frame_number;
    uint32_t timecode;
    uint32_t flags;
    uint64_t wall_us;
    uint32_t audio_bytes;
    uint32_t video_bytes;
    uint32_t payload_crc; ///< CRC-32C of audio then video
    uint8_t  reserved[16];
    uint32_t header_crc; ///< CRC-32C of every byte before it
};
static_assert(sizeof(record_header) == 64, "record header is 64 bytes");

struct index_header
{
    char     magic[8];
    uint32_t version;
    uint32_t header_bytes;
    uint32_t segment_index;
    uint8_t  reserved[64 - 8 - 4 * 3 - 4];
    uint32_t header_crc;
};
static_assert(sizeof(index_header) == 64, "index header is 64 bytes");

struct index_entry
{
    int64_t  frame_number;
    uint64_t offset;
    uint32_t record_bytes;
    uint32_t flags;
    uint64_t wall_us; ///< the contract calls this field `reserved`; it carries the record's wall time
};
static_assert(sizeof(index_entry) == 32, "index entry is 32 bytes");
#pragma pack(pop)

// ── CRC-32C ─────────────────────────────────────────────────────────────────────────────────
//
// Castagnoli, not the zlib polynomial: SSE4.2 computes it in hardware at several GB/s, which is
// what makes a CRC over every payload affordable on the recorder's worker. A 2 MB frame at 50p is
// 100 MB/s; a table CRC would spend a tenth of a core on it.

#ifndef REPLAY_HAVE_SSE42_CRC
uint32_t crc32c_sw(uint32_t crc, const uint8_t* p, size_t n)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}
#endif

REPLAY_TARGET_SSE42 uint32_t crc32c_update(uint32_t crc, const void* data, size_t n)
{
    auto p = static_cast<const uint8_t*>(data);
#ifdef REPLAY_HAVE_SSE42_CRC
    uint64_t c = crc;
    while (n >= 8) {
        uint64_t v;
        std::memcpy(&v, p, 8);
        c = _mm_crc32_u64(c, v);
        p += 8;
        n -= 8;
    }
    crc = static_cast<uint32_t>(c);
    while (n--)
        crc = _mm_crc32_u8(crc, *p++);
    return crc;
#else
    return crc32c_sw(crc, p, n);
#endif
}

uint32_t crc32c(const void* data, size_t n) { return ~crc32c_update(0xFFFFFFFFu, data, n); }

uint64_t now_us()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string os_error()
{
#ifdef _WIN32
    char buf[256] = {};
    strerror_s(buf, sizeof(buf), errno);
    return buf;
#else
    return std::strerror(errno);
#endif
}

std::string six(int i)
{
    std::string s = std::to_string(i);
    return std::string(s.size() < 6 ? 6 - s.size() : 0, '0') + s;
}

boost::filesystem::path seg_path_of(const boost::filesystem::path& dir, const std::string& stem, int i)
{
    return dir / (stem + "." + six(i) + ".rseg");
}
boost::filesystem::path idx_path_of(const boost::filesystem::path& dir, const std::string& stem, int i)
{
    return dir / (stem + "." + six(i) + ".ridx");
}
boost::filesystem::path manifest_path_of(const boost::filesystem::path& dir, const std::string& stem)
{
    return dir / (stem + ".rman.json");
}

/// `<stem>.NNNNNN.rseg` -> NNNNNN, or -1.
int segment_number(const std::string& filename, const std::string& stem, const char* ext)
{
    const std::string prefix = stem + ".";
    const std::string suffix = ext;
    if (filename.size() != prefix.size() + 6 + suffix.size() || filename.rfind(prefix, 0) != 0 ||
        filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)
        return -1;
    int v = 0;
    for (size_t i = prefix.size(); i < prefix.size() + 6; ++i) {
        if (filename[i] < '0' || filename[i] > '9')
            return -1;
        v = v * 10 + (filename[i] - '0');
    }
    return v;
}

std::FILE* open_write(const boost::filesystem::path& p)
{
#ifdef _WIN32
    // Readers open with every share mode; the writer has to allow them in.
    return _wfsopen(p.wstring().c_str(), L"wb", _SH_DENYNO);
#else
    return std::fopen(p.string().c_str(), "wb");
#endif
}

int64_t file_size(std::FILE* f)
{
#ifdef _WIN32
    const auto pos = _ftelli64(f);
    _fseeki64(f, 0, SEEK_END);
    const auto size = _ftelli64(f);
    _fseeki64(f, pos, SEEK_SET);
    return size;
#else
    const auto pos = ftello(f);
    fseeko(f, 0, SEEK_END);
    const auto size = ftello(f);
    fseeko(f, pos, SEEK_SET);
    return size;
#endif
}

bool seek_to(std::FILE* f, uint64_t offset)
{
#ifdef _WIN32
    return _fseeki64(f, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

/// Pushes the OS's buffers for this file to the device. Called when a segment CLOSES, not per
/// record: per-record sync would put the disk's worst-case latency on the recorder's worker. A
/// power loss can therefore cost the records written since the last roll -- which the CRC then
/// rejects rather than decodes.
void sync_file(std::FILE* f)
{
    std::fflush(f);
#ifdef _WIN32
    _commit(_fileno(f));
#else
    ::fsync(fileno(f));
#endif
}

void fill_segment_header(segment_header& h, int index, const format& f)
{
    std::memset(&h, 0, sizeof(h));
    std::memcpy(h.magic, kSegmentMagic, 8);
    h.version        = kVersion;
    h.header_bytes   = sizeof(h);
    h.segment_index  = static_cast<uint32_t>(index);
    h.codec          = static_cast<uint32_t>(f.video_codec);
    h.width          = f.width;
    h.height         = f.height;
    h.rate_num       = f.rate_num;
    h.rate_den       = f.rate_den;
    h.field_count    = f.field_count;
    h.bit_depth      = f.bit_depth;
    h.color_space    = f.color_space;
    h.color_transfer = f.color_transfer;
    h.audio_channels = f.audio_channels;
    h.sample_rate    = f.sample_rate;
    h.vmx_profile    = f.vmx_profile;
    h.created_us     = now_us();
    h.header_crc     = crc32c(&h, offsetof(segment_header, header_crc));
}

bool format_from_header(const segment_header& h, format& f)
{
    if (std::memcmp(h.magic, kSegmentMagic, 8) != 0 || h.header_bytes != sizeof(h) ||
        h.header_crc != crc32c(&h, offsetof(segment_header, header_crc)))
        return false;
    f.video_codec    = static_cast<codec>(h.codec);
    f.width          = h.width;
    f.height         = h.height;
    f.rate_num       = h.rate_num;
    f.rate_den       = h.rate_den;
    f.field_count    = h.field_count;
    f.bit_depth      = h.bit_depth;
    f.color_space    = h.color_space;
    f.color_transfer = h.color_transfer;
    f.audio_channels = h.audio_channels;
    f.sample_rate    = h.sample_rate;
    f.vmx_profile    = h.vmx_profile;
    return f.width > 0 && f.height > 0 && f.width <= 16384 && f.height <= 16384 && f.rate_num > 0 && f.rate_den > 0;
}

bool header_valid(const record_header& h)
{
    return h.magic == kRecordMagic && h.header_bytes == sizeof(record_header) &&
           h.header_crc == crc32c(&h, offsetof(record_header, header_crc)) &&
           static_cast<uint64_t>(h.audio_bytes) + h.video_bytes < (512ull << 20);
}

void write_manifest(const boost::filesystem::path& p, const format& f)
{
    // Written by hand: boost's property-tree JSON writer quotes EVERY value, so `"version": "2"`
    // -- a manifest whose numbers a client has to parse twice. The reader below still reads it.
    const auto t_now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm    tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &t_now);
#else
    gmtime_r(&t_now, &tm_utc);
#endif
    char iso[32];
    std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    std::ostringstream os;
    os << "{\n"
       << "  \"version\": 2,\n"
       << "  \"codec\": \"" << codec_name(f.video_codec) << "\",\n"
       << "  \"width\": " << f.width << ",\n"
       << "  \"height\": " << f.height << ",\n"
       << "  \"rate\": [" << f.rate_num << ", " << f.rate_den << "],\n"
       << "  \"field_count\": " << f.field_count << ",\n"
       << "  \"bit_depth\": " << f.bit_depth << ",\n"
       << "  \"color_space\": " << f.color_space << ",\n"
       << "  \"color_transfer\": " << f.color_transfer << ",\n"
       << "  \"audio_channels\": " << f.audio_channels << ",\n"
       << "  \"sample_rate\": " << f.sample_rate << ",\n"
       << "  \"vmx_profile\": " << f.vmx_profile << ",\n"
       << "  \"created\": \"" << iso << "\"\n"
       << "}\n";
    // Written whole and renamed into place, so a reader never sees half a manifest.
    const auto tmp = p.string() + ".tmp";
    if (auto* fp = std::fopen(tmp.c_str(), "wb")) {
        const auto s = os.str();
        std::fwrite(s.data(), 1, s.size(), fp);
        sync_file(fp);
        std::fclose(fp);
        boost::system::error_code ec;
        boost::filesystem::rename(tmp, p, ec);
    }
}

} // namespace

const char* codec_name(codec c)
{
    switch (c) {
        case codec::vmx_bgra8:
            return "vmx-bgra8";
        case codec::vmx_p216:
            return "vmx-p216";
        case codec::prores:
            return "prores";
    }
    return "unknown";
}

std::optional<codec> codec_from_name(const std::string& name)
{
    if (name == "vmx-bgra8")
        return codec::vmx_bgra8;
    if (name == "vmx-p216")
        return codec::vmx_p216;
    if (name == "prores")
        return codec::prores;
    return std::nullopt;
}

std::FILE* open_shared_read(const boost::filesystem::path& p)
{
#ifdef _WIN32
    // FILE_SHARE_DELETE is the point: retention deletes a segment a player is reading, and the
    // v1 store's `_fsopen(_SH_DENYNO)` did not grant it -- so `remove` failed, cleanup STOPPED
    // ("wait for the reader"), and a buffer being watched grew past its retention for as long as
    // anyone watched it.
    HANDLE h = ::CreateFileW(p.wstring().c_str(),
                             GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return nullptr;
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        ::CloseHandle(h);
        return nullptr;
    }
    std::FILE* f = _fdopen(fd, "rb");
    if (!f)
        _close(fd);
    return f;
#else
    return std::fopen(p.string().c_str(), "rb");
#endif
}

boost::filesystem::path recording_dir(const std::wstring& name)
{
    boost::filesystem::path p(name);
    if (!p.is_absolute())
        p = boost::filesystem::path(env::media_folder()) / p;
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  writer
// ─────────────────────────────────────────────────────────────────────────────────────────────

writer::~writer() { close(); }

bool writer::open(const boost::filesystem::path& dir, const format& fmt, int64_t segment_records, int64_t retain_records)
{
    close();
    error_          = {};
    dir_            = dir;
    stem_           = dir.filename().string();
    fmt_            = fmt;
    segment_records_ = std::max<int64_t>(1, segment_records);
    retain_records_  = std::max<int64_t>(0, retain_records);

    boost::system::error_code ec;
    boost::filesystem::create_directories(dir_, ec);
    if (ec && !boost::filesystem::is_directory(dir_)) {
        fail("cannot create " + dir_.string() + ": " + ec.message());
        return false;
    }
    write_manifest(manifest_path_of(dir_, stem_), fmt_);
    return roll();
}

void writer::fail(const std::string& what)
{
    if (!error_)
        error_.what = what;
    close_current(false);
}

bool writer::roll()
{
    close_current(true);

    segment s;
    s.index    = next_index_++;
    s.seg_path = seg_path_of(dir_, stem_, s.index);
    s.idx_path = idx_path_of(dir_, stem_, s.index);

    seg_file_ = open_write(s.seg_path);
    if (!seg_file_) {
        fail("segment open failed: " + s.seg_path.string() + ": " + os_error());
        return false;
    }
    idx_file_ = open_write(s.idx_path);
    if (!idx_file_) {
        fail("index open failed: " + s.idx_path.string() + ": " + os_error());
        return false;
    }

    segment_header sh;
    fill_segment_header(sh, s.index, fmt_);
    index_header ih{};
    std::memcpy(ih.magic, kIndexMagic, 8);
    ih.version       = kVersion;
    ih.header_bytes  = sizeof(ih);
    ih.segment_index = static_cast<uint32_t>(s.index);
    ih.header_crc    = crc32c(&ih, offsetof(index_header, header_crc));

    if (std::fwrite(&sh, sizeof(sh), 1, seg_file_) != 1 || std::fflush(seg_file_) != 0) {
        fail("segment write failed: " + s.seg_path.string() + ": " + os_error());
        return false;
    }
    if (std::fwrite(&ih, sizeof(ih), 1, idx_file_) != 1 || std::fflush(idx_file_) != 0) {
        fail("index write failed: " + s.idx_path.string() + ": " + os_error());
        return false;
    }
    seg_offset_ = sizeof(sh);
    segments_.push_back(s);
    return true;
}

void writer::close_current(bool sync)
{
    if (seg_file_) {
        if (sync)
            sync_file(seg_file_);
        std::fclose(seg_file_);
        seg_file_ = nullptr;
    }
    if (idx_file_) {
        if (sync)
            sync_file(idx_file_);
        std::fclose(idx_file_);
        idx_file_ = nullptr;
    }
}

bool writer::append(const record_meta& meta, const void* audio, size_t audio_bytes, const void* video, size_t video_bytes)
{
    if (error_)
        return false;
    if (segments_.empty() || segments_.back().records >= segment_records_) {
        if (!roll())
            return false;
    }
    auto& cur = segments_.back();

    record_header h{};
    h.magic        = kRecordMagic;
    h.header_bytes = sizeof(h);
    h.frame_number = meta.frame_number;
    h.timecode     = meta.timecode;
    h.flags        = meta.flags;
    h.wall_us      = meta.wall_us;
    h.audio_bytes  = static_cast<uint32_t>(audio_bytes);
    h.video_bytes  = static_cast<uint32_t>(video_bytes);
    uint32_t c     = 0xFFFFFFFFu;
    if (audio_bytes)
        c = crc32c_update(c, audio, audio_bytes);
    if (video_bytes)
        c = crc32c_update(c, video, video_bytes);
    h.payload_crc = ~c;
    h.header_crc  = crc32c(&h, offsetof(record_header, header_crc));

    // One write call for the whole record: fewer syscalls and, on a failure part way, a record
    // that no index entry will ever point at.
    const size_t total = sizeof(h) + audio_bytes + video_bytes;
    scratch_.resize(total);
    std::memcpy(scratch_.data(), &h, sizeof(h));
    if (audio_bytes)
        std::memcpy(scratch_.data() + sizeof(h), audio, audio_bytes);
    if (video_bytes)
        std::memcpy(scratch_.data() + sizeof(h) + audio_bytes, video, video_bytes);

    if (std::fwrite(scratch_.data(), 1, total, seg_file_) != total || std::fflush(seg_file_) != 0) {
        fail("segment write failed: " + cur.seg_path.string() + ": " + os_error());
        return false;
    }

    // THE INDEX ENTRY ONLY NOW, after the record's bytes have reached the OS. A reader trusting
    // the index therefore never reaches a record that is not all there.
    index_entry e{};
    e.frame_number = meta.frame_number;
    e.offset       = seg_offset_;
    e.record_bytes = static_cast<uint32_t>(total);
    e.flags        = meta.flags;
    e.wall_us      = meta.wall_us;
    if (std::fwrite(&e, sizeof(e), 1, idx_file_) != 1 || std::fflush(idx_file_) != 0) {
        fail("index write failed: " + cur.idx_path.string() + ": " + os_error());
        return false;
    }

    seg_offset_ += total;
    cur.records++;
    held_records_++;
    records_++;
    retire_old();
    return true;
}

void writer::retire_old()
{
    if (retain_records_ <= 0)
        return;
    // Whole segments, oldest first, while what would remain still covers the retention window.
    while (segments_.size() > 1 && held_records_ - segments_.front().records >= retain_records_) {
        const auto old = segments_.front();
        boost::system::error_code ec1, ec2;
        // Payload first, index second: a reader that sees the index gone treats the segment as
        // gone, and one that sees only the payload gone fails its read and drops it.
        boost::filesystem::remove(old.seg_path, ec1);
        boost::filesystem::remove(old.idx_path, ec2);
        if (ec1 && boost::filesystem::exists(old.seg_path)) {
            CASPAR_LOG(warning) << L"[replay] could not delete " << old.seg_path.wstring() << L": "
                                << u16(ec1.message()) << L"; retention will retry";
            return;
        }
        held_records_ -= old.records;
        segments_.pop_front();
    }
}

void writer::close()
{
    close_current(true);
    segments_.clear();
    held_records_ = 0;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  reader
// ─────────────────────────────────────────────────────────────────────────────────────────────

reader::~reader() { close(); }

bool reader::is_v2(const boost::filesystem::path& dir)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(dir, ec))
        return false;
    const auto stem = dir.filename().string();
    if (boost::filesystem::exists(manifest_path_of(dir, stem), ec))
        return true;
    for (boost::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (segment_number(it->path().filename().string(), stem, ".rseg") >= 0)
            return true;
    return false;
}

void reader::close()
{
    if (open_file_) {
        std::fclose(open_file_);
        open_file_ = nullptr;
    }
    open_index_ = -1;
    segments_.clear();
    count_ = 0;
}

bool reader::open(const boost::filesystem::path& dir)
{
    close();
    dir_  = dir;
    stem_ = dir.filename().string();

    std::vector<int> found;
    boost::system::error_code ec;
    for (boost::filesystem::directory_iterator it(dir_, ec), end; !ec && it != end; it.increment(ec)) {
        const int n = segment_number(it->path().filename().string(), stem_, ".rseg");
        if (n >= 0)
            found.push_back(n);
    }
    std::sort(found.begin(), found.end());

    bool have_format = false;
    for (int n : found) {
        segment s;
        s.index    = n;
        s.seg_path = seg_path_of(dir_, stem_, n);
        s.idx_path = idx_path_of(dir_, stem_, n);
        if (!have_format) {
            if (auto* f = open_shared_read(s.seg_path)) {
                segment_header h;
                if (std::fread(&h, sizeof(h), 1, f) == 1)
                    have_format = format_from_header(h, fmt_);
                std::fclose(f);
            }
        }
        if (load_segment(s))
            segments_.push_back(std::move(s));
    }
    // A recording ADDed a moment ago may have a manifest and no segment yet.
    if (!have_format) {
        boost::system::error_code mec;
        const auto mp = manifest_path_of(dir_, stem_);
        if (boost::filesystem::exists(mp, mec)) {
            try {
                boost::property_tree::ptree t;
                boost::property_tree::read_json(mp.string(), t);
                if (auto c = codec_from_name(t.get<std::string>("codec", ""))) {
                    fmt_.video_codec = *c;
                    fmt_.width       = t.get<int>("width", 0);
                    fmt_.height      = t.get<int>("height", 0);
                    auto rate        = t.get_child("rate");
                    auto r           = rate.begin();
                    fmt_.rate_num    = r->second.get_value<int>();
                    fmt_.rate_den    = (++r)->second.get_value<int>();
                    fmt_.field_count = t.get<int>("field_count", 1);
                    fmt_.bit_depth   = t.get<int>("bit_depth", 8);
                    fmt_.color_space = t.get<int>("color_space", 0);
                    fmt_.color_transfer = t.get<int>("color_transfer", 0);
                    fmt_.audio_channels = t.get<int>("audio_channels", 0);
                    fmt_.sample_rate    = t.get<int>("sample_rate", 48000);
                    fmt_.vmx_profile    = t.get<uint32_t>("vmx_profile", 0);
                    have_format = fmt_.width > 0 && fmt_.height > 0 && fmt_.rate_num > 0 && fmt_.rate_den > 0;
                }
            } catch (const std::exception&) {
                have_format = false;
            }
        }
    }

    // Renumber, then count.
    count_ = 0;
    for (auto& s : segments_) {
        s.first_position = count_;
        count_ += static_cast<int64_t>(s.entries.size());
    }
    return have_format;
}

bool reader::load_segment(segment& s)
{
    s.entries.clear();
    s.idx_bytes_read = sizeof(index_header);
    s.scan_offset    = sizeof(segment_header);
    extend_segment(s);
    return true;
}

void reader::extend_segment(segment& s)
{
    // 1. The index: whole entries beyond what was read, in order. An entry for a record the scan
    //    below already found (offset < scan_offset) is skipped -- and so is one whose bytes are
    //    NOT all in the payload file. The writer never indexes a record before flushing it, so
    //    that happens only to a file truncated after the fact (a crash, a copy cut short), and a
    //    record that is not all there is not part of the recording (W8).
    const std::FILE* pf    = payload(s);
    const int64_t    psize = pf ? file_size(const_cast<std::FILE*>(pf)) : 0;
    if (auto* f = open_shared_read(s.idx_path)) {
        const int64_t size = file_size(f);
        if (size >= static_cast<int64_t>(s.idx_bytes_read + sizeof(index_entry)) && seek_to(f, s.idx_bytes_read)) {
            const size_t n = static_cast<size_t>((size - static_cast<int64_t>(s.idx_bytes_read)) / sizeof(index_entry));
            std::vector<index_entry> buf(n);
            const size_t got = std::fread(buf.data(), sizeof(index_entry), n, f);
            for (size_t i = 0; i < got; ++i) {
                const auto& e = buf[i];
                if (e.offset < s.scan_offset)
                    continue;
                if (e.offset != s.scan_offset)
                    break; // a gap means the index disagrees with what we know: stop and let the scan decide
                if (static_cast<int64_t>(e.offset + e.record_bytes) > psize)
                    break; // indexed but not all on disk: torn
                s.entries.push_back({e.frame_number, e.offset, e.record_bytes, e.flags, e.wall_us});
                s.scan_offset = e.offset + e.record_bytes;
            }
            s.idx_bytes_read += got * sizeof(index_entry);
        }
        std::fclose(f);
    }

    // 2. The payload past the last known record: records the writer flushed but has not indexed
    //    yet, or ever (a crash between the two writes). Each must validate WHOLE -- header CRC,
    //    size within the file, payload CRC -- and the scan stops at the first that does not,
    //    which is how a torn tail stays out of the recording.
    std::FILE* f = payload(s);
    if (!f)
        return;
    const int64_t size = file_size(f);
    std::vector<uint8_t> body;
    while (static_cast<int64_t>(s.scan_offset + sizeof(record_header)) <= size) {
        record_header h;
        if (!seek_to(f, s.scan_offset) || std::fread(&h, sizeof(h), 1, f) != 1 || !header_valid(h))
            break;
        const uint64_t bytes = sizeof(h) + uint64_t(h.audio_bytes) + h.video_bytes;
        if (static_cast<int64_t>(s.scan_offset + bytes) > size)
            break;
        body.resize(h.audio_bytes + size_t(h.video_bytes));
        if (!body.empty() && std::fread(body.data(), 1, body.size(), f) != body.size())
            break;
        if ((body.empty() ? 0u : crc32c(body.data(), body.size())) != (body.empty() ? 0u : h.payload_crc))
            break;
        s.entries.push_back({h.frame_number, s.scan_offset, static_cast<uint32_t>(bytes), h.flags, h.wall_us});
        s.scan_offset += bytes;
    }
}

std::FILE* reader::payload(segment& s)
{
    if (open_file_ && open_index_ == s.index)
        return open_file_;
    if (open_file_) {
        std::fclose(open_file_);
        open_file_ = nullptr;
    }
    open_file_  = open_shared_read(s.seg_path);
    open_index_ = open_file_ ? s.index : -1;
    return open_file_;
}

void reader::refresh()
{
    if (dir_.empty())
        return;

    // Segments retention removed: gone when the payload is gone (removed first by the writer).
    bool dropped = false;
    while (!segments_.empty()) {
        boost::system::error_code ec;
        if (boost::filesystem::exists(segments_.front().seg_path, ec))
            break;
        if (open_index_ == segments_.front().index && open_file_) {
            std::fclose(open_file_);
            open_file_  = nullptr;
            open_index_ = -1;
        }
        retired_ += static_cast<int64_t>(segments_.front().entries.size());
        segments_.pop_front();
        dropped = true;
    }

    if (segments_.empty()) {
        open(dir_);
        return;
    }

    extend_segment(segments_.back());

    // New segments after the last one held.
    for (int n = segments_.back().index + 1;; ++n) {
        boost::system::error_code ec;
        const auto p = seg_path_of(dir_, stem_, n);
        if (!boost::filesystem::exists(p, ec))
            break;
        // The previous segment is closed now; pick up any tail it gained before it closed.
        extend_segment(segments_.back());
        segment s;
        s.index    = n;
        s.seg_path = p;
        s.idx_path = idx_path_of(dir_, stem_, n);
        load_segment(s);
        segments_.push_back(std::move(s));
    }

    (void)dropped;
    count_ = 0;
    for (auto& s : segments_) {
        s.first_position = count_;
        count_ += static_cast<int64_t>(s.entries.size());
    }
}

bool reader::meta_at(int64_t position, record_meta& meta) const
{
    if (position < 0 || position >= count_)
        return false;
    auto it = std::upper_bound(segments_.begin(), segments_.end(), position,
                               [](int64_t p, const segment& s) { return p < s.first_position; });
    if (it == segments_.begin())
        return false;
    --it;
    const auto local = static_cast<size_t>(position - it->first_position);
    if (local >= it->entries.size())
        return false;
    const auto& e     = it->entries[local];
    meta.frame_number = e.frame_number;
    meta.flags        = e.flags;
    meta.wall_us      = e.wall_us;
    meta.timecode     = timecode_unknown;
    return true;
}

bool reader::read(int64_t position, record_meta& meta, std::vector<uint8_t>& audio, std::vector<uint8_t>& video)
{
    audio.clear();
    video.clear();
    if (position < 0 || position >= count_)
        return false;
    auto it = std::upper_bound(segments_.begin(), segments_.end(), position,
                               [](int64_t p, const segment& s) { return p < s.first_position; });
    if (it == segments_.begin())
        return false;
    --it;
    const auto local = static_cast<size_t>(position - it->first_position);
    if (local >= it->entries.size())
        return false;
    const auto& e = it->entries[local];

    std::FILE* f = payload(*it);
    if (!f)
        return false;
    record_header h;
    if (!seek_to(f, e.offset) || std::fread(&h, sizeof(h), 1, f) != 1 || !header_valid(h) ||
        sizeof(h) + uint64_t(h.audio_bytes) + h.video_bytes != e.record_bytes) {
        ++bad_records_;
        return false;
    }
    audio.resize(h.audio_bytes);
    video.resize(h.video_bytes);
    if ((h.audio_bytes && std::fread(audio.data(), 1, audio.size(), f) != audio.size()) ||
        (h.video_bytes && std::fread(video.data(), 1, video.size(), f) != video.size())) {
        ++bad_records_;
        audio.clear();
        video.clear();
        return false;
    }
    uint32_t c = 0xFFFFFFFFu;
    if (h.audio_bytes)
        c = crc32c_update(c, audio.data(), audio.size());
    if (h.video_bytes)
        c = crc32c_update(c, video.data(), video.size());
    if (~c != h.payload_crc) {
        ++bad_records_;
        audio.clear();
        video.clear();
        return false;
    }
    meta.frame_number = h.frame_number;
    meta.timecode     = h.timecode;
    meta.flags        = h.flags;
    meta.wall_us      = h.wall_us;
    return true;
}

int64_t reader::position_of_frame(int64_t frame_number) const
{
    for (const auto& s : segments_) {
        if (s.entries.empty() || s.entries.back().frame_number < frame_number)
            continue;
        auto it = std::lower_bound(s.entries.begin(), s.entries.end(), frame_number,
                                   [](const entry& e, int64_t f) { return e.frame_number < f; });
        return s.first_position + static_cast<int64_t>(it - s.entries.begin());
    }
    return count_ > 0 ? count_ - 1 : 0;
}

int64_t reader::position_of_wall(uint64_t wall_us) const
{
    for (const auto& s : segments_) {
        if (s.entries.empty() || s.entries.back().wall_us < wall_us)
            continue;
        auto it = std::lower_bound(s.entries.begin(), s.entries.end(), wall_us,
                                   [](const entry& e, uint64_t t) { return e.wall_us < t; });
        return s.first_position + static_cast<int64_t>(it - s.entries.begin());
    }
    return count_ > 0 ? count_ - 1 : 0;
}

}}} // namespace caspar::replay::store
