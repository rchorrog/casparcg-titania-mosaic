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

// THE REPLAY STORE, VERSION 2 (docs/plans/REPLAY_V2_PLAN.md).
//
// A recording is a directory `media/<name>/` holding numbered PAYLOAD segments (`.rseg`), one
// sidecar INDEX per segment (`.ridx`) and a JSON MANIFEST (`.rman.json`).
//
// THE PAYLOAD IS THE TRUTH; THE INDEX IS A CACHE. Every record carries its own header -- magic,
// frame number, timecode, wall-clock time, sizes, and a CRC32 of the header and of the payload --
// so a recording can be read with no index at all, a torn tail is recognised rather than decoded,
// and an index entry is written only once its record is flushed. The v1 store (`.mav`/`.idx`)
// had none of this: the index held only an offset and a timestamp, a frame's size was the
// distance to the NEXT offset or to END OF FILE, and the last frame of a growing recording was
// therefore read together with whatever part of the next payload had reached the disk.
//
// FRAME NUMBERS, NOT TIMESTAMPS, ARE THE KEY. Phase 1 numbers records by the tick that produced
// them; phase 2 replaces that with a shared frame clock so that recordings made on one server, or
// on genlocked servers, share one numbering. The wall-clock time is kept for display and for the
// `yyyy-mm-dd-hh-mm-ss-ff` addressing the module has always offered.

#pragma once

#include <boost/filesystem.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace caspar { namespace replay { namespace store {

enum class codec : uint32_t
{
    vmx_bgra8 = 1, ///< VMX from 8-bit BGRA (libvmx converts with its BT.601/709 matrix)
    vmx_p216  = 2, ///< VMX from 10-bit 4:2:2 Y'CbCr, MSB-aligned in 16 bits (P216); matrix is the manifest's
    prores    = 3, ///< reserved for phase 3
};

const char* codec_name(codec c);
std::optional<codec> codec_from_name(const std::string& name);

/// What every record of one recording shares. Written into the manifest and into every segment
/// header, so a segment found on its own is still self-describing.
struct format
{
    codec    video_codec    = codec::vmx_bgra8;
    int      width          = 0;
    int      height         = 0;
    int      rate_num       = 25; ///< the channel's frame rate as a rational, never a rounded float
    int      rate_den       = 1;
    int      field_count    = 1;  ///< 2 for an interlaced channel: one record per FIELD tick
    int      bit_depth      = 8;
    int      color_space    = 0;  ///< core::color_space as an integer
    int      color_transfer = 0;  ///< core::color_transfer as an integer
    int      audio_channels = 0;
    int      sample_rate    = 48000;
    uint32_t vmx_profile    = 0;

    /// Records per second: the field rate on an interlaced channel, because `send` is called per field.
    double ticks_per_second() const { return rate_den > 0 ? double(rate_num) * field_count / rate_den : 25.0; }
};

/// Per-record metadata, as written and as read back.
struct record_meta
{
    int64_t  frame_number = 0;
    uint32_t timecode     = 0; ///< SMPTE BCD hh:mm:ss:ff, 0xFFFFFFFF when unknown (phase 2 fills it)
    uint32_t flags        = 0;
    uint64_t wall_us      = 0; ///< microseconds since the Unix epoch, at the tick
};

constexpr uint32_t timecode_unknown = 0xFFFFFFFFu;

/// Why a writer stopped, or empty.
struct write_error
{
    std::string what;
    explicit operator bool() const { return !what.empty(); }
};

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  Writer
// ─────────────────────────────────────────────────────────────────────────────────────────────

class writer
{
  public:
    writer() = default;
    ~writer();
    writer(const writer&)            = delete;
    writer& operator=(const writer&) = delete;

    /// `dir` is the recording's own directory (created); files are `dir/<stem>.*`.
    /// `segment_records` rolls a segment; `retain_records` deletes whole segments from the front
    /// once the recording holds more than that (0 = keep everything).
    bool open(const boost::filesystem::path& dir, const format& fmt, int64_t segment_records, int64_t retain_records);

    /// Appends one record. FALSE means the recording has failed and will accept nothing more;
    /// `error()` says why. A record is either wholly in the payload with its index entry after it,
    /// or -- on a failure part way -- not indexed and rejected by any reader's CRC check.
    bool append(const record_meta& meta, const void* audio, size_t audio_bytes, const void* video, size_t video_bytes);

    /// Flushes to the OS and closes. Safe to call twice.
    void close();

    const write_error& error() const { return error_; }
    int64_t            records() const { return records_; }
    int                segments_held() const { return static_cast<int>(segments_.size()); }

  private:
    struct segment
    {
        int                     index = 0;
        boost::filesystem::path seg_path;
        boost::filesystem::path idx_path;
        int64_t                 records = 0;
    };

    bool roll();
    void close_current(bool sync);
    void retire_old();
    void fail(const std::string& what);

    boost::filesystem::path dir_;
    std::string             stem_;
    format                  fmt_;
    int64_t                 segment_records_ = 0;
    int64_t                 retain_records_  = 0;

    std::FILE*          seg_file_ = nullptr;
    std::FILE*          idx_file_ = nullptr;
    uint64_t            seg_offset_ = 0;
    std::deque<segment> segments_;
    int64_t             held_records_ = 0;
    int64_t             records_      = 0;
    int                 next_index_   = 0;
    write_error         error_;
    std::vector<uint8_t> scratch_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  Reader
// ─────────────────────────────────────────────────────────────────────────────────────────────

/// Reads a v2 recording, including one that is still being written and one whose front is being
/// deleted by retention. NOT thread-safe: one reader per thread (the producer's decode thread owns
/// one; export opens its own).
class reader
{
  public:
    reader() = default;
    ~reader();
    reader(const reader&)            = delete;
    reader& operator=(const reader&) = delete;

    /// True for a directory that holds a v2 recording (a manifest or at least one `.rseg`).
    static bool is_v2(const boost::filesystem::path& dir);

    bool open(const boost::filesystem::path& dir);
    void close();

    /// Picks up records appended since the last call and segments retention has removed.
    /// Cheap enough for every tick: it reads only index bytes beyond what it has, plus a
    /// validated scan of the payload past the last index entry.
    void refresh();

    const format& fmt() const { return fmt_; }

    /// Records currently readable, numbered 0..count()-1 from the oldest still held.
    int64_t count() const { return count_; }

    /// Reads record `position`. FALSE if it does not exist or does not validate (counted in
    /// `bad_records()`), in which case `video`/`audio` are left empty.
    bool read(int64_t position, record_meta& meta, std::vector<uint8_t>& audio, std::vector<uint8_t>& video);

    /// The metadata of record `position`, from the index -- no payload read.
    bool meta_at(int64_t position, record_meta& meta) const;

    /// The first position whose wall-clock time is >= `wall_us` (clamped to the held range).
    int64_t position_of_wall(uint64_t wall_us) const;

    /// The first position whose frame number is >= `frame_number` (clamped to the held range).
    /// Frame numbers never change once written, positions do -- retention renumbers them every
    /// time it removes a segment from the front -- so a player keeps its place by frame number.
    int64_t position_of_frame(int64_t frame_number) const;

    /// Records that failed validation since open.
    int64_t bad_records() const { return bad_records_; }

    /// Records retention has removed from the front since open. `position + retired()` is a
    /// stable serial for a record, which is how a player reports a position that stays right
    /// while the buffer rolls under it.
    int64_t retired() const { return retired_; }

    const boost::filesystem::path& dir() const { return dir_; }

  private:
    struct entry
    {
        int64_t  frame_number = 0;
        uint64_t offset       = 0;
        uint32_t record_bytes = 0;
        uint32_t flags        = 0;
        uint64_t wall_us      = 0;
    };
    struct segment
    {
        int                     index = 0;
        boost::filesystem::path seg_path;
        boost::filesystem::path idx_path;
        std::vector<entry>      entries;
        uint64_t                idx_bytes_read = 0; ///< index file bytes consumed so far
        uint64_t                scan_offset    = 0; ///< payload offset after the last known record
        int64_t                 first_position = 0;
    };

    bool load_segment(segment& s);
    void extend_segment(segment& s);
    std::FILE* payload(segment& s);

    boost::filesystem::path dir_;
    std::string             stem_;
    format                  fmt_;
    std::deque<segment>     segments_;
    int64_t                 count_       = 0;
    int64_t                 bad_records_ = 0;
    int64_t                 retired_     = 0;

    std::FILE* open_file_  = nullptr;
    int        open_index_ = -1;
};

/// `media/<name>` for a requested name, or the name itself when it is already a path.
boost::filesystem::path recording_dir(const std::wstring& name);

/// Opens a file for reading with every share mode, DELETE included, so a writer's retention can
/// remove a segment a reader holds open -- the v1 store could not, and its cleanup stopped.
std::FILE* open_shared_read(const boost::filesystem::path& p);

}}} // namespace caspar::replay::store
