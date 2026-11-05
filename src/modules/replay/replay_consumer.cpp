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
 * Derived from the CasparCG replay module
 * (https://github.com/krzyc/CasparCG-Server/tree/master/src/modules/replay).
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 * Copyright (c) 2013 Technical University of Lodz Multimedia Centre <office@cm.p.lodz.pl>
 * Authors: Robert Nagy <ronag89@gmail.com>,
 *          Jan Starzak <jan@ministryofgoodsteps.com>,
 *          Krzysztof Pyrkosz <pyrkosz@o2.pl>
 */

#include "replay_consumer.h"

#include "vmxcodec.h"

#include <common/env.h>
#include <common/except.h>
#include <common/future.h>
#include <common/log.h>
#include <common/utf.h>
#include <core/consumer/channel_info.h>

#include <boost/algorithm/string.hpp>

#include <chrono>
#include <iomanip>
#include <sstream>

namespace caspar { namespace replay {

namespace {

uint64_t wall_now_us()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

} // namespace

record_request parse_record_request(const std::wstring& name_with_query, const std::wstring& quality)
{
    record_request req;

    std::string path  = u8(name_with_query);
    const auto  q_pos = path.find('?');
    if (q_pos != std::string::npos) {
        const std::string query = path.substr(q_pos + 1);
        path                    = path.substr(0, q_pos);

        // EXACT KEYS, STRICT VALUES, and anything else REFUSED -- unchanged from v1. A replay
        // buffer is a promise about how far back an operator can go; a typo must not quietly
        // change it.
        const auto whole_number = [](const std::string& key, const std::string& s) -> int {
            std::size_t used = 0;
            long        v    = 0;
            try {
                v = std::stol(s, &used);
            } catch (const std::exception&) {
                used = 0;
            }
            if (s.empty() || used != s.size() || v <= 0 || v > 10L * 24 * 3600)
                CASPAR_THROW_EXCEPTION(user_error()
                                       << msg_info("REPLAY ?" + key + "= takes a positive whole number, not '" + s + "'"));
            return static_cast<int>(v);
        };
        const auto duration = [&](const std::string& key, const std::string& s, int unit) -> int {
            const auto colon = s.find(':');
            if (colon == std::string::npos)
                return whole_number(key, s) * unit;
            const auto h = s.substr(0, colon);
            const auto m = s.substr(colon + 1);
            if (m.find(':') != std::string::npos)
                CASPAR_THROW_EXCEPTION(user_error()
                                       << msg_info("REPLAY ?" + key + "= takes HH:MM or a whole number, not '" + s + "'"));
            const int hours   = h == "0" || h == "00" ? 0 : whole_number(key, h);
            const int minutes = m == "0" || m == "00" ? 0 : whole_number(key, m);
            if (minutes >= 60 || hours * 60 + minutes <= 0)
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(
                                           "REPLAY ?" + key +
                                           "= takes HH:MM with minutes below 60 and a length above zero, not '" + s + "'"));
            return (hours * 60 + minutes) * 60;
        };

        std::vector<std::string> pairs;
        boost::split(pairs, query, boost::is_any_of("&"));
        bool have_max = false;
        for (const auto& kv : pairs) {
            if (kv.empty())
                continue;
            const auto eq = kv.find('=');
            if (eq == std::string::npos)
                CASPAR_THROW_EXCEPTION(user_error() << msg_info("REPLAY option '" + kv + "' has no value"));
            const auto key = kv.substr(0, eq);
            const auto val = kv.substr(eq + 1);
            if (key == "max_duration") {
                req.max_duration_sec = duration(key, val, 1);
                have_max             = true;
            } else if (key == "duration") {
                if (!have_max)
                    req.max_duration_sec = duration(key, val, 60);
            } else if (key == "segment_duration" || key == "segment") {
                req.segment_sec = whole_number(key, val);
            } else {
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(
                                           "REPLAY option '" + key +
                                           "' is not one of max_duration, duration, segment_duration, segment"));
            }
        }
    }
    boost::trim_right_if(path, boost::is_any_of("/\\"));
    if (path.size() > 4 && boost::iends_with(path, ".mav"))
        path.resize(path.size() - 4);
    if (path.empty())
        CASPAR_THROW_EXCEPTION(user_error() << msg_info("REPLAY needs a recording name"));
    req.name = u16(path);

    if (!quality.empty()) {
        const auto& q = quality;
        if (boost::iequals(q, L"LQ"))
            req.vmx_profile = VMX_PROFILE_LQ;
        else if (boost::iequals(q, L"SQ"))
            req.vmx_profile = VMX_PROFILE_SQ;
        else if (boost::iequals(q, L"HQ"))
            req.vmx_profile = VMX_PROFILE_HQ;
        else if (boost::iequals(q, L"OMT_LQ"))
            req.vmx_profile = VMX_PROFILE_OMT_LQ;
        else if (boost::iequals(q, L"OMT_SQ"))
            req.vmx_profile = VMX_PROFILE_OMT_SQ;
        else if (boost::iequals(q, L"OMT_HQ"))
            req.vmx_profile = VMX_PROFILE_OMT_HQ;
        else
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(
                                       "REPLAY quality must be LQ, SQ, HQ, OMT_LQ, OMT_SQ or OMT_HQ, not '" + u8(q) + "'"));
    }
    return req;
}

int replay_consumer::index_for(const std::wstring& name)
{
    // FNV-1a over the lower-cased UTF-8 name. Stable across runs and processes, which REMOVE
    // needs: it rebuilds the consumer from the command's own parameters to find this number.
    uint32_t   h = 2166136261u;
    const auto s = boost::to_lower_copy(u8(name));
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return 200000 + static_cast<int>(h % 9999u);
}

replay_consumer::replay_consumer(record_request req)
    : req_(std::move(req))
    , index_(index_for(req_.name))
{
    // NOTHING ELSE. No directory is created and nothing is renamed until `initialize`, because a
    // REMOVE constructs a consumer too -- and every REMOVE used to run the overwrite rename.
    graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
    graph_->set_color("queue", diagnostics::color(0.9f, 0.9f, 0.1f));
    graph_->set_text(print());
    diagnostics::register_graph(graph_);
}

replay_consumer::~replay_consumer()
{
    // Drain, then join, then close: the frames in the queue are frames the operator believes are
    // recorded, and the worker holds the encoder and the writer.
    stop_.store(true, std::memory_order_release);
    queue_cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    if (writer_)
        writer_->close();
}

std::wstring replay_consumer::print() const
{
    return L"replay[" + (dir_.empty() ? req_.name : dir_.wstring()) + L"]";
}

void replay_consumer::initialize(const core::video_format_desc& format_desc,
                                 const core::channel_info&      channel_info,
                                 int /*port_index*/)
{
    // A recording cannot continue across a change of raster: refused on a second call, so
    // `output` stops this consumer and the destructor keeps what was recorded.
    if (worker_.joinable() || writer_)
        CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info(
                                   "replay: the channel format changed during a recording; the recording is stopped and "
                                   "kept. ADD a new REPLAY consumer to record at the new format."));

    // ── Where. The overwrite protection lives HERE now, not in the constructor. ──
    dir_ = store::recording_dir(req_.name);
    {
        boost::system::error_code ec;
        const bool taken = boost::filesystem::exists(dir_, ec) &&
                           (store::reader::is_v2(dir_) || !boost::filesystem::is_empty(dir_, ec));
        // The flat v1 layout beside it counts as taken too: a v1 reader would splice them.
        const bool v1_flat = boost::filesystem::exists(dir_.string() + ".mav.000", ec);
        if (taken || v1_flat) {
            const auto now = std::chrono::system_clock::now();
            const auto t   = std::chrono::system_clock::to_time_t(now);
            std::tm    tm_now{};
#ifdef _WIN32
            localtime_s(&tm_now, &t);
#else
            localtime_r(&t, &tm_now);
#endif
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
            std::ostringstream ss;
            ss << "_" << std::put_time(&tm_now, "%Y%m%d_%H%M%S") << "_" << std::setfill('0') << std::setw(3)
               << ms.count();
            const auto renamed = dir_.parent_path() / (dir_.filename().string() + ss.str());
            CASPAR_LOG(warning) << print() << L" a recording called " << dir_.filename().wstring()
                                << L" exists; this one is recorded as " << renamed.filename().wstring()
                                << L" so the first is not overwritten.";
            dir_ = renamed;
        }
    }

    // ── What. ──
    const bool is_16bit   = channel_info.depth != common::bit_depth::bit8;
    fmt_.video_codec      = codec_for_depth(is_16bit);
    fmt_.width            = format_desc.width;
    fmt_.height           = format_desc.height;
    fmt_.rate_num         = format_desc.framerate.numerator();
    fmt_.rate_den         = format_desc.framerate.denominator();
    fmt_.field_count      = format_desc.field_count;
    fmt_.bit_depth        = is_16bit ? 16 : 8;
    fmt_.color_space      = static_cast<int>(channel_info.default_color_space);
    fmt_.color_transfer   = 0; // CasparCG 2.5.0 has no transfer tag; 0 is SDR in the store header
    fmt_.audio_channels   = format_desc.audio_channels;
    fmt_.sample_rate      = format_desc.audio_sample_rate;
    fmt_.vmx_profile      = req_.vmx_profile ? req_.vmx_profile : static_cast<uint32_t>(VMX_PROFILE_SQ);

    encoder_ = make_encoder(fmt_);
    writer_  = std::make_unique<store::writer>();
    const double tps = fmt_.ticks_per_second();
    if (!writer_->open(dir_,
                       fmt_,
                       static_cast<int64_t>(std::llround(req_.segment_sec * tps)),
                       static_cast<int64_t>(std::llround(req_.max_duration_sec * tps)))) {
        fail(writer_->error().what);
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_["file/path"]     = dir_.wstring();
        state_["replay/codec"]  = std::string(store::codec_name(fmt_.video_codec));
    }
    publish();
    graph_->set_text(print());

    if (!failed_)
        worker_ = std::thread([this] { worker_loop(); });
}

std::future<bool> replay_consumer::send(core::video_field /*field*/, core::const_frame frame)
{
    // EVERY tick advances the frame number, recorded or not, so a dropped frame is a visible gap
    // in the numbering rather than a silent splice.
    const int64_t tick = ticks_++;

    // A FAILED recording keeps answering `true`: `false` makes `output` erase this consumer with
    // no log line, and the reason the operator needs to read is in this consumer's state.
    if (failed_.load(std::memory_order_acquire) || !writer_)
        return make_ready_future(true);

    const auto& data = frame.image_data(0);
    if (data.size() == 0 || data.data() == nullptr)
        return make_ready_future(true);

    pending p;
    // STAMPED HERE, on the channel thread, at the tick -- not on the worker after a queue up to
    // four deep, which is what put about four frames of jitter between two recordings' timestamps.
    p.meta.frame_number = tick;
    p.meta.wall_us      = wall_now_us();
    p.meta.timecode     = store::timecode_unknown;

    const auto& desc     = frame.pixel_format_desc();
    p.image.is_16bit     = !desc.planes.empty() && desc.planes[0].depth != common::bit_depth::bit8;
    p.image.rgba         = desc.format == core::pixel_format::rgba;
    p.image.stride       = desc.planes.empty() ? 0 : desc.planes[0].linesize;
    if (p.image.stride <= 0 && fmt_.height > 0)
        p.image.stride = static_cast<int>(data.size() / static_cast<size_t>(fmt_.height));
    p.image.bytes.assign(data.data(), data.data() + data.size());
    const auto& audio = frame.audio_data();
    p.audio.assign(audio.begin(), audio.end());

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queue_.size() >= queue_capacity_) {
            // Drop, never block: blocking puts the disk's worst case on the channel thread.
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            if (!drop_warned_) {
                drop_warned_ = true;
                CASPAR_LOG(warning) << print()
                                    << L" write queue full; dropping frames. The volume is not keeping up with the "
                                       L"channel. Logged once; counted in `replay/dropped_frames`.";
            }
            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
            return make_ready_future(true);
        }
        queue_.push_back(std::move(p));
        graph_->set_value("queue", static_cast<double>(queue_.size()) / queue_capacity_);
    }
    queue_cv_.notify_one();
    return make_ready_future(true);
}

void replay_consumer::worker_loop()
{
    while (true) {
        pending p;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return stop_.load(std::memory_order_acquire) || !queue_.empty(); });
            if (queue_.empty())
                return; // drained after stop
            p = std::move(queue_.front());
            queue_.pop_front();
        }
        if (failed_.load(std::memory_order_acquire))
            continue; // keep draining so the destructor can join; nothing more is written
        try {
            write_one(p);
        } catch (const std::exception&) {
            // std::exception only: under /EHa a catch-all would also eat access violations.
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
    }
}

void replay_consumer::write_one(pending& p)
{
    if (!encoder_->encode(p.image, encoded_)) {
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        if (!encode_warned_) {
            encode_warned_ = true;
            CASPAR_LOG(warning) << print() << L" the " << u16(std::string(store::codec_name(fmt_.video_codec)))
                                << L" encoder rejected a frame; skipping it and continuing. Logged once.";
        }
        publish();
        return;
    }
    if (!writer_->append(p.meta,
                         p.audio.data(),
                         p.audio.size() * sizeof(int32_t),
                         encoded_.data(),
                         encoded_.size())) {
        fail(writer_->error().what);
        return;
    }
    frames_written_.fetch_add(1, std::memory_order_relaxed);
    graph_->set_value("frame-time", 0.5);
    publish();
}

void replay_consumer::fail(const std::string& why)
{
    if (failed_.exchange(true))
        return;
    // LOUD, ONCE, AND IN THE STATE. The v1 writer set a flag on a failed segment roll and the
    // worker ignored the `false` it then returned: recording stopped with `frames_written`
    // still counting and nothing in the log.
    CASPAR_LOG(error) << print() << L" RECORDING STOPPED: " << u16(why)
                      << L". Nothing more is written to this recording; ADD a new REPLAY consumer to record again.";
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        error_ = why;
    }
    graph_->set_tag(diagnostics::tag_severity::WARNING, "failed");
    publish();
}

void replay_consumer::publish()
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto written = frames_written_.load(std::memory_order_relaxed);
    const auto dropped = frames_dropped_.load(std::memory_order_relaxed);
    state_["replay/state"]          = std::string(failed_ ? "failed" : "recording");
    state_["replay/error"]          = error_;
    state_["replay/frames_written"] = written;
    state_["replay/dropped_frames"] = dropped;
    state_["replay/segments"]       = writer_ ? writer_->segments_held() : 0;
    // The v1 keys, kept: batteries and clients read them.
    state_["frames_written"] = written;
    state_["dropped_frames"] = dropped;
}

core::monitor::state replay_consumer::state() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

}} // namespace caspar::replay
