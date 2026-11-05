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

#include "replay_source.h"

#include "replay_segmented_storage.h"

#include <core/frame/pixel_format.h>

#include <cmath>
#include <cstring>

namespace caspar { namespace replay {

namespace {

class v2_source final : public source
{
    store::reader r_;

  public:
    bool open(const boost::filesystem::path& dir) { return r_.open(dir); }

    const store::format& fmt() const override { return r_.fmt(); }
    void                 refresh() override { r_.refresh(); }
    int64_t              count() const override { return r_.count(); }
    bool read(int64_t p, store::record_meta& m, std::vector<uint8_t>& a, std::vector<uint8_t>& v) override
    {
        return r_.read(p, m, a, v);
    }
    bool    meta_at(int64_t p, store::record_meta& m) const override { return r_.meta_at(p, m); }
    int64_t position_of_frame(int64_t f) const override { return r_.position_of_frame(f); }
    int64_t position_of_wall(uint64_t w) const override { return r_.position_of_wall(w); }
    int64_t bad_records() const override { return r_.bad_records(); }
    int64_t retired() const override { return r_.retired(); }
    bool    is_legacy() const override { return false; }
};

/// The v1 layout, read-only. A v1 record is `[uint32 audio bytes][audio][VMX]`, sized by the
/// distance to the next index offset -- with every weakness that implies (W8). Kept so recordings
/// made before replay v2 still play; v1 has no stable frame numbers, so its POSITIONS stand in for
/// them, and a v1 buffer still rolling under retention renumbers as it always did.
class v1_source final : public source
{
    mutable ReplaySegmentedReader r_;
    store::format                 fmt_;

  public:
    bool open(const boost::filesystem::path& base)
    {
        if (!r_.Open(base))
            return false;
        fmt_.video_codec = store::codec::vmx_bgra8;
        fmt_.width       = r_.GetWidth();
        fmt_.height      = r_.GetHeight();
        // v1 stored a double; 29.97/59.94 come back as the 1001 rationals they were.
        const double fps = r_.GetFps();
        if (std::abs(fps - std::round(fps)) > 0.001) { // 29.97 -> 30000/1001, 59.94 -> 60000/1001
            fmt_.rate_num = static_cast<int>(std::lround(fps * 1.001)) * 1000;
            fmt_.rate_den = 1001;
        } else {
            fmt_.rate_num = static_cast<int>(std::lround(fps));
            fmt_.rate_den = 1;
        }
        fmt_.field_count = 1;
        fmt_.bit_depth   = 8;
        fmt_.color_space = static_cast<int>(core::color_space::bt709);
        return fmt_.width > 0 && fmt_.height > 0;
    }

    const store::format& fmt() const override { return fmt_; }
    void                 refresh() override { r_.Refresh(); }
    int64_t              count() const override { return static_cast<int64_t>(r_.GetTotalFrames()); }
    bool read(int64_t p, store::record_meta& m, std::vector<uint8_t>& a, std::vector<uint8_t>& v) override
    {
        a.clear();
        v.clear();
        std::vector<uint8_t> blob;
        uint64_t             ts = 0;
        if (p < 0 || !r_.GetFrame(static_cast<size_t>(p), blob, ts) || blob.size() < 4)
            return false;
        uint32_t ab = 0;
        std::memcpy(&ab, blob.data(), 4);
        if (blob.size() < 4ull + ab)
            return false;
        a.assign(blob.begin() + 4, blob.begin() + 4 + ab);
        v.assign(blob.begin() + 4 + ab, blob.end());
        m.frame_number = p;
        m.wall_us      = ts;
        m.timecode     = store::timecode_unknown;
        m.flags        = 0;
        return true;
    }
    bool meta_at(int64_t p, store::record_meta& m) const override
    {
        uint64_t ts = 0;
        if (p < 0 || !r_.GetTimestamp(static_cast<size_t>(p), ts))
            return false;
        m.frame_number = p;
        m.wall_us      = ts;
        m.timecode     = store::timecode_unknown;
        return true;
    }
    int64_t position_of_frame(int64_t f) const override
    {
        const auto n = count();
        return f < 0 ? 0 : (f >= n ? (n > 0 ? n - 1 : 0) : f);
    }
    int64_t position_of_wall(uint64_t w) const override { return static_cast<int64_t>(r_.SeekTimestamp(w)); }
    int64_t bad_records() const override { return 0; }
    int64_t retired() const override { return 0; }
    bool    is_legacy() const override { return true; }
};

bool v1_present(const boost::filesystem::path& p)
{
    // Both v1 layouts: `media/<name>/<name>.idx.NNN` and the older flat `media/<name>.idx.NNN`.
    const auto holds = [](const boost::filesystem::path& dir, const std::string& stem) {
        boost::system::error_code ec;
        if (!boost::filesystem::is_directory(dir, ec))
            return false;
        for (boost::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().filename().string().rfind(stem + ".idx.", 0) == 0)
                return true;
        return false;
    };
    const auto name = p.filename().string();
    return holds(p, name) || holds(p.parent_path(), name);
}

} // namespace

bool is_recording(const std::wstring& name)
{
    const auto dir = store::recording_dir(name);
    return store::reader::is_v2(dir) || v1_present(dir);
}

std::unique_ptr<source> open_source(const std::wstring& name)
{
    const auto dir = store::recording_dir(name);
    if (store::reader::is_v2(dir)) {
        auto s = std::make_unique<v2_source>();
        if (s->open(dir))
            return s;
        return nullptr;
    }
    if (v1_present(dir)) {
        auto s = std::make_unique<v1_source>();
        if (s->open(dir))
            return s;
    }
    return nullptr;
}

}} // namespace caspar::replay
