/*
 * Copyright (c) 2025 CasparCG Contributors
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
 *
 * This module uses libvmx (https://github.com/openmediatransport/libvmx),
 * licensed under MIT, which is compatible with GPL-3.
 *
 * The .mav/.idx file format and segmented storage strategy are derived from
 * the CasparCG replay module
 * (https://github.com/krzyc/CasparCG-Server/tree/master/src/modules/replay).
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 * Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
 * Authors: Jan Starzak <jan@ministryofgoodsteps.com>,
 *          Krzysztof Pyrkosz <pyrkosz@o2.pl>
 */

// THE v1 STORE, READ-ONLY. Recordings made before replay v2 (`.mav.NNN` + `.idx.NNN`) still play
// through this reader; nothing writes this layout any more (see replay_store.h for why).

#include "replay_segmented_storage.h"
#include <common/log.h>

#include <iostream>
#include <algorithm>
#include <cstdio>
#include <boost/lexical_cast.hpp>
#include <string>
#include <vector>

#ifdef _WIN32
#include <share.h> // For _fsopen
#endif

#ifdef _WIN32
#define FSEEK64 _fseeki64
#define FTELL64 _ftelli64
#else
#define FSEEK64 fseeko
#define FTELL64 ftello
#endif

namespace caspar { namespace replay {

// ----------------------------------------------------------------------------
// ReplaySegmentedReader
// ----------------------------------------------------------------------------

ReplaySegmentedReader::ReplaySegmentedReader()
    : total_frames_(0)
    , total_duration_(0)
    , cached_file_mav_(nullptr)
    , cached_segment_index_(-1)
{
}

ReplaySegmentedReader::~ReplaySegmentedReader()
{
    Close();
}

bool ReplaySegmentedReader::Open(const boost::filesystem::path& base_path)
{
    Close();
    base_path_ = base_path;
    namespace fs = boost::filesystem;
    
    fs::path parent;
    std::string base_name;
    std::string file_prefix;
    
    // Check new folder structure: path is a directory (or path base without ext is directory)
    fs::path p = base_path;
    if (p.has_extension()) p.replace_extension("");
    
    if (fs::exists(p) && fs::is_directory(p)) {
        parent = p;
        base_name = p.filename().string();
        // Files inside will be named like folder
        file_prefix = (p / base_name).string();
    } else {
        // Old structure: files in parent folder
        parent = base_path.parent_path();
        base_name = base_path.filename().string();
        file_prefix = base_path.string();
        if (base_path.has_extension()) {
             // If base_path was passed with extension (e.g. file.mav), strip it for prefix
             fs::path temp = base_path;
             temp.replace_extension("");
             file_prefix = temp.string();
             base_name = temp.filename().string();
        }
    }
    
    if (!fs::exists(parent)) return false;

    std::vector<SegmentReadInfo> discovered;

    try {
        for (fs::directory_iterator it(parent); it != fs::directory_iterator(); ++it) {
            std::string filename = it->path().filename().string();
            // Match base_name.idx.00000
            if (filename.find(base_name + ".idx.") == 0) {
                // Parse index
                std::string idx_part = filename.substr((base_name + ".idx.").length());
                int index = std::stoi(idx_part);
                
                SegmentReadInfo info;
                info.index = index;
                info.idx_path = it->path();
                info.mav_path = file_prefix + ".mav." + idx_part;
                
                if (fs::exists(info.mav_path)) {
                    discovered.push_back(info);
                }
            }
        }
    } catch (...) {
        return false;
    }

    // Sort by index
    std::sort(discovered.begin(), discovered.end(), [](const SegmentReadInfo& a, const SegmentReadInfo& b) {
        return a.index < b.index;
    });

    // Load indices
    total_frames_ = 0;
    for (auto& seg : discovered) {
#ifdef _WIN32
        FILE* f = _fsopen(seg.idx_path.string().c_str(), "rb", _SH_DENYNO);
#else
        FILE* f = fopen(seg.idx_path.string().c_str(), "rb");
#endif
        if (!f) continue;
        
        // Read Header
        replay_segment_header header;
        if (fread(&header, sizeof(header), 1, f) != 1) {
            fclose(f);
            continue;
        }
        
        // Basic sanity check — reject obviously corrupt headers
        if (header.width == 0 || header.width > 16384 ||
            header.height == 0 || header.height > 16384 ||
            header.fps <= 0.0 || header.fps > 1000.0) {
            CASPAR_LOG(warning) << L"Replay: Skipping segment with invalid header (w=" 
                                << header.width << L" h=" << header.height << L" fps=" << header.fps << L")";
            fclose(f);
            continue;
        }
    
        width_ = header.width;
        height_ = header.height;
        fps_ = header.fps;

        FSEEK64(f, 0, SEEK_END);
        long long size = FTELL64(f);
        FSEEK64(f, sizeof(header), SEEK_SET);
        
        long long entry_count = (size - (long long)sizeof(header)) / (long long)sizeof(replay_index_entry_v2);
        if (entry_count > 0) {
            seg.indices.resize((size_t)entry_count);
            size_t read_count = fread(seg.indices.data(), sizeof(replay_index_entry_v2), (size_t)entry_count, f);
            if (read_count < (size_t)entry_count) {
                // A short read leaves the unread tail zero-filled (from resize
                // above) — truncate instead of treating those zero
                // timestamp/offset entries as real data.
                seg.indices.resize(read_count);
            }
            if (seg.indices.empty()) {
                seg.start_timestamp = seg.end_timestamp = 0;
            } else {
            // Correct timestamps from entries
            seg.start_timestamp = seg.indices.front().timestamp;
            seg.end_timestamp = seg.indices.back().timestamp;
            }
            
            seg.global_start_frame = total_frames_;
            // What was READ, not what the file size promised: after a short read above the
            // tail was truncated away, and counting `entry_count` put frames on the timeline
            // that no index entry backs -- a seek there read past the segment's indices.
            total_frames_ += seg.indices.size();
            segments_.push_back(seg);
        }
        fclose(f);
    }
    
    if (!segments_.empty()) {
        total_duration_ = segments_.back().end_timestamp - segments_.front().start_timestamp;
    }

    return !segments_.empty();
}

void ReplaySegmentedReader::Close()
{
    if (cached_file_mav_) {
        fclose(cached_file_mav_);
        cached_file_mav_ = nullptr;
    }
    cached_segment_index_ = -1;
    segments_.clear();
}


bool ReplaySegmentedReader::GetTimestamp(size_t index, uint64_t& timestamp)
{
    if (index >= total_frames_) return false;

    // Find Segment by global index
    auto it = std::upper_bound(segments_.begin(), segments_.end(), index, 
        [](size_t idx, const SegmentReadInfo& seg) {
            return idx < seg.global_start_frame;
        });
    
    if (it == segments_.begin()) return false;
    auto& seg = *(--it);

    size_t local_index = index - seg.global_start_frame;
    if (local_index >= seg.indices.size()) return false;

    timestamp = seg.indices[local_index].timestamp;
    return true;
}

bool ReplaySegmentedReader::GetFrame(size_t index, std::vector<uint8_t>& data, uint64_t& timestamp)
{
    if (index >= total_frames_) return false;

    // Find Segment
    auto it = std::upper_bound(segments_.begin(), segments_.end(), index, 
        [](size_t idx, const SegmentReadInfo& seg) {
            return idx < seg.global_start_frame;
        });
    
    if (it == segments_.begin()) return false;
    auto& seg = *(--it);

    size_t local_index = index - seg.global_start_frame;
    if (local_index >= seg.indices.size()) return false;

    const auto& entry = seg.indices[local_index];
    timestamp = entry.timestamp;

    // Open MAV file
    if (cached_segment_index_ != seg.index) {
        if (cached_file_mav_) fclose(cached_file_mav_);
#ifdef _WIN32
        cached_file_mav_ = _fsopen(seg.mav_path.string().c_str(), "rb", _SH_DENYNO);
#else
        cached_file_mav_ = fopen(seg.mav_path.string().c_str(), "rb");
#endif
        cached_segment_index_ = seg.index;
    }
    if (!cached_file_mav_) return false;

    // Determine size (next offset - this offset)
    int64_t next_offset;
    if (local_index + 1 < seg.indices.size()) {
        next_offset = seg.indices[local_index + 1].file_offset;
    } else {
        // End of file
        FSEEK64(cached_file_mav_, 0, SEEK_END);
        next_offset = FTELL64(cached_file_mav_);
    }
    
    if (next_offset <= entry.file_offset) return false;
    
    // Safety cap for frame size (e.g. 100MB) to prevent length_error on corruption
    int64_t diff = next_offset - entry.file_offset;
    if (diff > 100 * 1024 * 1024) return false;

    size_t size = (size_t)diff;
    // Ensure vector is exactly the size of the frame data
    if (data.size() != size) data.resize(size);
    
    FSEEK64(cached_file_mav_, entry.file_offset, SEEK_SET);
    if (fread(data.data(), 1, size, cached_file_mav_) != size) return false;

    return true;
}

size_t ReplaySegmentedReader::SeekTimestamp(uint64_t timestamp)
{
    // Find segment
    for(const auto& seg : segments_) {
        if (timestamp >= seg.start_timestamp && timestamp <= seg.end_timestamp) {
            // Binary search inside segment indices
            auto it = std::lower_bound(seg.indices.begin(), seg.indices.end(), timestamp,
                [](const replay_index_entry_v2& entry, uint64_t ts) {
                    return entry.timestamp < ts;
                });
            
            size_t local = std::distance(seg.indices.begin(), it);
            if (local >= seg.indices.size()) local = seg.indices.size() - 1;
            return seg.global_start_frame + local;
        }
    }
    // Handle out of bounds
    if (segments_.empty()) return 0;
    if (timestamp < segments_.front().start_timestamp) return 0;
    return total_frames_ - 1;
}

size_t ReplaySegmentedReader::GetTotalFrames() const { return total_frames_; }

void ReplaySegmentedReader::Refresh() 
{
    // Invalidate cached MAV handle — segments may have been deleted or changed
    if (cached_file_mav_) {
        fclose(cached_file_mav_);
        cached_file_mav_ = nullptr;
    }
    cached_segment_index_ = -1;

    if (segments_.empty()) {
        Open(base_path_);
        return;
    }

    // ── SEGMENTS THE WRITER HAS DELETED GO FIRST ────────────────────────────────────────
    //
    // Retention deletes from the FRONT while this reader only ever appended: `segments_` was
    // cleared or pushed to and never erased, so a recording's timeline grew forever. An operator
    // seeking to "the start" landed in material deleted minutes ago, and the number a client
    // draws its scrub bar from was fiction.
    //
    // With the defaults -- a 24-hour buffer in 60-second segments -- this needs a day of
    // recording to show. A sports buffer of a few minutes hits it in a few minutes, which is
    // what the feature is FOR.
    //
    // Checked against the `.idx`, not the `.mav`: the writer removes the payload first and the
    // index second, so a segment whose index is gone is definitively gone, while one whose
    // payload has just gone might still be mid-cleanup.
    {
        std::size_t dropped = 0;
        while (!segments_.empty()) {
            boost::system::error_code ec;
            if (boost::filesystem::exists(segments_.front().idx_path, ec))
                break;
            dropped += segments_.front().indices.size();
            segments_.erase(segments_.begin());
        }
        if (dropped > 0) {
            total_frames_ = total_frames_ > dropped ? total_frames_ - dropped : 0;
            // The global frame numbering is relative to the first segment held, so everything
            // after a drop has to be renumbered or every seek lands in the wrong place.
            std::size_t running = 0;
            for (auto& seg : segments_) {
                seg.global_start_frame = running;
                running += seg.indices.size();
            }
        }
    }

    if (segments_.empty()) {
        Open(base_path_);
        return;
    }

    // Incremental refresh:
    // 1. Update last segment
    // 2. Scan for next segments
    
    // --- Update Last Segment ---
    auto& last_seg = segments_.back();
#ifdef _WIN32
    FILE* f = _fsopen(last_seg.idx_path.string().c_str(), "rb", _SH_DENYNO);
#else
    FILE* f = fopen(last_seg.idx_path.string().c_str(), "rb");
#endif
    if (f) {
        long long current_count = (long long)last_seg.indices.size();
        long long offset = sizeof(replay_segment_header) + (current_count * sizeof(replay_index_entry_v2));
        
        FSEEK64(f, 0, SEEK_END);
        long long file_size = FTELL64(f);
       
        if (file_size > offset) {
            FSEEK64(f, offset, SEEK_SET);
            long long new_entries_bytes = file_size - offset;
            long long new_count = new_entries_bytes / sizeof(replay_index_entry_v2);
            
            if (new_count > 0) {
                std::vector<replay_index_entry_v2> buffer(new_count);
                if (fread(buffer.data(), sizeof(replay_index_entry_v2), new_count, f) == (size_t)new_count) {
                    last_seg.indices.insert(last_seg.indices.end(), buffer.begin(), buffer.end());
                    if (!last_seg.indices.empty()) {
                        last_seg.end_timestamp = last_seg.indices.back().timestamp;
                    }
                    total_frames_ += (size_t)new_count;
                }
            }
        }
        fclose(f);
    }

    // --- Check for Next Segments ---
    // Infer prefix from last segment path
    std::string path_str = last_seg.idx_path.string();
    size_t last_dot_idx = path_str.rfind(".idx.");
    if (last_dot_idx == std::string::npos) return; 
    
    std::string prefix = path_str.substr(0, last_dot_idx);
    int next_idx = last_seg.index + 1;
    
    while (true) {
        // Try likely padding formats (001, 1)
        std::vector<std::string> trials;
        std::string p3 = std::to_string(next_idx);
        while (p3.length() < 3) p3 = "0" + p3;
        trials.push_back(p3);
        
        std::string p_raw = std::to_string(next_idx);
        if (p_raw != p3) trials.push_back(p_raw);
        
        boost::filesystem::path next_idx_path;
        bool found = false;
        
        for (const auto& s : trials) {
            boost::filesystem::path p = prefix + ".idx." + s;
            if (boost::filesystem::exists(p)) {
                next_idx_path = p;
                found = true;
                break;
            }
        }
        
        if (!found) break; // No next segment found

        // Found new segment -> Verify MAV exists
        std::string mav_s = next_idx_path.string();
        size_t pos = mav_s.rfind(".idx.");
        if (pos != std::string::npos) mav_s.replace(pos, 5, ".mav.");
        
        if (!boost::filesystem::exists(mav_s)) break; // Index without Mav
        
        // Open and Read new segment
#ifdef _WIN32
        FILE* f2 = _fsopen(next_idx_path.string().c_str(), "rb", _SH_DENYNO);
#else
        FILE* f2 = fopen(next_idx_path.string().c_str(), "rb");
#endif      
        if (!f2) break;
        
        replay_segment_header h;
        if (fread(&h, sizeof(h), 1, f2) != 1) { fclose(f2); break; }
        
        FSEEK64(f2, 0, SEEK_END);
        long long sz = FTELL64(f2);
        FSEEK64(f2, sizeof(h), SEEK_SET);
        
        long long count = (sz - sizeof(h)) / sizeof(replay_index_entry_v2);
        if (count > 0) {
            SegmentReadInfo new_seg;
            new_seg.index = next_idx;
            new_seg.idx_path = next_idx_path;
            new_seg.mav_path = mav_s;
            
            new_seg.indices.resize(count);
            size_t read_count = fread(new_seg.indices.data(), sizeof(replay_index_entry_v2), count, f2);
            if (read_count < (size_t)count) {
                new_seg.indices.resize(read_count);
                count = (long long)read_count;
            }
            if (!new_seg.indices.empty()) {
                new_seg.start_timestamp = new_seg.indices.front().timestamp;
                new_seg.end_timestamp = new_seg.indices.back().timestamp;
            }
            new_seg.global_start_frame = total_frames_;

            total_frames_ += (size_t)count;
            segments_.push_back(new_seg);
        }
        fclose(f2);
        
        next_idx++;
        // Continue loop to find next segment...
    }
    
    if (!segments_.empty()) {
        total_duration_ = segments_.back().end_timestamp - segments_.front().start_timestamp;
    }
}

uint64_t ReplaySegmentedReader::GetDuration() const { return total_duration_; }

int ReplaySegmentedReader::GetWidth() const { return width_; }
int ReplaySegmentedReader::GetHeight() const { return height_; }
double ReplaySegmentedReader::GetFps() const { return fps_; }

}}
