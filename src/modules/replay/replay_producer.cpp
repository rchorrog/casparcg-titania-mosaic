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

#include "replay_producer.h"

#include <common/except.h>
#include <common/future.h>
#include <common/log.h>
#include <common/utf.h>
#include <core/frame/frame.h>
#include <core/frame/pixel_format.h>

#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>

namespace caspar { namespace replay {

namespace {

/// How many ticks ahead the reader decodes. At 50p that is 160 ms of lead over the playhead --
/// more than a VMX decode takes at 1080p by an order of magnitude -- and 8 frames of memory.
constexpr int kLookahead = 8;
constexpr size_t kCacheCap = 24;

[[noreturn]] void refuse(const std::wstring& what)
{
    CASPAR_THROW_EXCEPTION(caspar::user_error() << msg_info(u8(L"[replay] " + what)));
}

bool parse_int(const std::wstring& s, int64_t& out)
{
    if (s.empty())
        return false;
    size_t used = 0;
    try {
        out = std::stoll(s, &used);
    } catch (const std::exception&) {
        return false;
    }
    return used == s.size();
}

bool parse_double(const std::wstring& s, double& out)
{
    if (s.empty())
        return false;
    size_t used = 0;
    try {
        out = std::stod(s, &used);
    } catch (const std::exception&) {
        return false;
    }
    return used == s.size() && std::isfinite(out);
}

/// `yyyy-mm-dd-hh-mm-ss-ff` (the module's own form, ff in frames at `fps`) or ISO 8601 with a
/// `T` or a space. Microseconds since the Unix epoch, or nullopt for anything else -- the v1
/// parser returned 0 on failure, and 0 was then used as a position.
std::optional<uint64_t> parse_wall(const std::wstring& ws, double fps)
{
    using namespace boost::posix_time;
    const std::string s = u8(ws);
    const ptime       epoch(boost::gregorian::date(1970, 1, 1));

    std::vector<std::string> parts;
    boost::split(parts, s, boost::is_any_of("-"));
    if (parts.size() == 7) {
        int v[7];
        for (int i = 0; i < 7; ++i) {
            size_t used = 0;
            try {
                v[i] = std::stoi(parts[i], &used);
            } catch (const std::exception&) {
                return std::nullopt;
            }
            if (used != parts[i].size())
                return std::nullopt;
        }
        try {
            const ptime pt(boost::gregorian::date(static_cast<unsigned short>(v[0]), static_cast<unsigned short>(v[1]),
                                                  static_cast<unsigned short>(v[2])),
                           time_duration(v[3], v[4], v[5]));
            uint64_t    us = static_cast<uint64_t>((pt - epoch).total_microseconds());
            if (fps > 0)
                us += static_cast<uint64_t>(v[6] / fps * 1e6);
            return us;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    try {
        std::string iso = s;
        const auto  t   = iso.find('T');
        if (t != std::string::npos)
            iso[t] = ' ';
        const ptime pt = time_from_string(iso);
        if (pt.is_not_a_date_time())
            return std::nullopt;
        return static_cast<uint64_t>((pt - epoch).total_microseconds());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::wstring format_wall(uint64_t us, double fps)
{
    if (us == 0)
        return L"0000-00-00-00-00-00-00";
    using namespace boost::posix_time;
    const ptime pt = ptime(boost::gregorian::date(1970, 1, 1)) + microseconds(static_cast<int64_t>(us));
    const tm    t  = to_tm(pt);
    const int   ff = fps > 0 ? static_cast<int>((us % 1000000) / 1e6 * fps) : 0;
    std::wstringstream w;
    w << std::setfill(L'0') << std::setw(4) << (t.tm_year + 1900) << L"-" << std::setw(2) << (t.tm_mon + 1) << L"-"
      << std::setw(2) << t.tm_mday << L"-" << std::setw(2) << t.tm_hour << L"-" << std::setw(2) << t.tm_min << L"-"
      << std::setw(2) << t.tm_sec << L"-" << std::setw(2) << ff;
    return w.str();
}

} // namespace

replay_producer::replay_producer(std::unique_ptr<source> src, std::wstring display_name,
                                 spl::shared_ptr<core::frame_factory> frame_factory)
    : display_name_(std::move(display_name))
    , frame_factory_(std::move(frame_factory))
    , src_(std::move(src))
{
    fmt_     = src_->fmt();
    decoder_ = make_decoder(fmt_);

    graph_->set_color("late-decode", diagnostics::color(1.0f, 0.3f, 0.3f));
    graph_->set_color("ahead", diagnostics::color(0.2f, 0.8f, 0.2f));
    graph_->set_text(print());
    diagnostics::register_graph(graph_);

    {
        std::lock_guard<std::mutex> io(io_mutex_);
        src_->refresh();
        store::record_meta first, last;
        const int64_t      n = src_->count();
        std::lock_guard<std::mutex> lock(transport_mutex_);
        t_.count = n;
        if (n > 0 && src_->meta_at(0, first) && src_->meta_at(n - 1, last)) {
            t_.first_fn = first.frame_number;
            t_.last_fn  = last.frame_number;
        }
        t_.retired = src_->retired();
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_["file/path"]    = display_name_;
        state_["replay/codec"] = std::string(store::codec_name(fmt_.video_codec));
        state_["replay/legacy"] = src_->is_legacy();
    }
    CASPAR_LOG(info) << print() << L" opened: " << fmt_.width << L"x" << fmt_.height << L" "
                     << fmt_.ticks_per_second() << L" records/s, " << u16(std::string(store::codec_name(fmt_.video_codec)))
                     << (src_->is_legacy() ? L" (v1 layout, read-only)" : L"");

    reader_ = std::thread([this] { reader_loop(); });
}

replay_producer::~replay_producer()
{
    stop_.store(true);
    reader_cv_.notify_all();
    if (reader_.joinable())
        reader_.join();
    if (export_thread_.joinable())
        export_thread_.join();
}

void replay_producer::kick()
{
    kick_.store(true, std::memory_order_release);
    reader_cv_.notify_one();
}

// ── THE TRANSPORT ARITHMETIC ─────────────────────────────────────────────────────────────────
//
// One function, used for the real tick and for the reader's prediction of the next ones.
// `next_fn` is the frame number the NEXT tick shows, so a seek to N shows N on the very next
// tick at every speed (W6): the v1 producer incremented BEFORE reading, so a seek while playing
// showed N+1 and PLAY never showed frame 0.
int64_t replay_producer::step(transport& t, bool& at_end)
{
    at_end = false;
    if (t.count <= 0 || t.last_fn < t.first_fn)
        return INT64_MIN;

    if (t.live) {
        // FOLLOW: always `live_distance` behind the newest record (W7). The v1 `LIVE` was a
        // one-shot seek to end-25 that then drifted with the playback clock.
        const int64_t s = std::clamp(t.last_fn - t.live_distance, t.first_fn, t.last_fn);
        t.next_fn       = static_cast<double>(s + 1);
        return s;
    }

    const int64_t lo = t.in_fn == INT64_MIN ? t.first_fn : std::clamp(t.in_fn, t.first_fn, t.last_fn);
    int64_t       hi = t.out_fn > 0 ? std::min(t.out_fn, t.last_fn + 1) : t.last_fn + 1; // exclusive
    if (hi <= lo)
        hi = lo + 1;

    double  n = t.next_fn;
    int64_t s = static_cast<int64_t>(std::floor(n));
    if (s >= hi) {
        if (t.loop) {
            s = lo;
        } else {
            s      = hi - 1;
            at_end = true;
            // Fast-forwarding into the end of a growing recording means "catch up to live":
            // continue at normal speed from the newest frame, as v1 did.
            if (t.speed > 1.0)
                t.speed = 1.0;
        }
        n = static_cast<double>(s);
    } else if (s < lo) {
        if (t.loop) {
            s = hi - 1;
        } else {
            s      = lo;
            at_end = t.speed < 0;
        }
        n = static_cast<double>(s);
    }
    t.next_fn = n + t.speed;
    return s;
}

int64_t replay_producer::resolve_position(int64_t position)
{
    const int64_t n = src_->count();
    store::record_meta m;
    if (n <= 0)
        return 0;
    if (position >= n) {
        src_->meta_at(n - 1, m);
        return m.frame_number + 1;
    }
    if (!src_->meta_at(std::max<int64_t>(0, position), m))
        return 0;
    return m.frame_number;
}

bool replay_producer::decode_into(int64_t fn, cached& out)
{
    const int64_t pos = src_->position_of_frame(fn);
    store::record_meta m;
    if (!src_->meta_at(pos, m))
        return false;
    // NEVER resolve a frame number past the newest record. `position_of_frame` clamps to the
    // last record, so a request for a frame not yet written would map -- and the mapping would
    // be cached -- to an OLDER record; when that frame number did arrive the playhead showed the
    // stale one. Measured: `LIVE 3` held seven records behind the head, not three.
    if (m.frame_number < fn)
        return false;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(m.frame_number);
        if (it != cache_.end()) {
            resolved_[fn] = m.frame_number;
            out           = it->second;
            return true;
        }
    }

    std::vector<uint8_t> audio, video;
    if (!src_->read(pos, m, audio, video))
        return false; // counted in bad_records by the store
    auto frame = decoder_->decode(video, *frame_factory_, this);
    if (!frame)
        return false;
    if (!audio.empty()) {
        std::vector<int32_t> samples(audio.size() / sizeof(int32_t));
        std::memcpy(samples.data(), audio.data(), samples.size() * sizeof(int32_t));
        frame->audio_data() = caspar::array<int32_t>(std::move(samples));
    }
    cached c;
    c.frame        = core::draw_frame(std::move(*frame));
    c.serial       = pos + src_->retired();
    c.frame_number = m.frame_number;
    c.wall_us      = m.wall_us;

    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_[m.frame_number] = c;
    resolved_[fn]          = m.frame_number;
    out                    = std::move(c);
    return true;
}

void replay_producer::reader_loop()
{
    while (!stop_.load()) {
        {
            std::unique_lock<std::mutex> lk(reader_cv_mutex_);
            reader_cv_.wait_for(lk, std::chrono::milliseconds(10), [this] { return stop_.load() || kick_.load(); });
            kick_.store(false);
        }
        if (stop_.load())
            break;

        try {
            // 1. What the recording holds now.
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                src_->refresh();
                const int64_t      n = src_->count();
                store::record_meta first, last;
                const bool         ok = n > 0 && src_->meta_at(0, first) && src_->meta_at(n - 1, last);
                std::lock_guard<std::mutex> lock(transport_mutex_);
                t_.count   = n;
                t_.retired = src_->retired();
                if (ok) {
                    t_.first_fn = first.frame_number;
                    t_.last_fn  = last.frame_number;
                }
            }

            // 2. The next ticks, by the same arithmetic the channel thread will use.
            std::vector<int64_t> wanted;
            {
                std::lock_guard<std::mutex> lock(transport_mutex_);
                transport p = t_;
                if (!p.started) {
                    p.next_fn = static_cast<double>(p.in_fn == INT64_MIN ? p.first_fn : p.in_fn);
                    p.started = true;
                }
                for (int i = 0; i < kLookahead; ++i) {
                    bool          e  = false;
                    const int64_t fn = step(p, e);
                    if (fn == INT64_MIN)
                        break;
                    if (wanted.empty() || wanted.back() != fn)
                        wanted.push_back(fn);
                }
            }

            // 3. Decode what is missing, nearest first.
            int64_t ready = 0;
            for (const int64_t fn : wanted) {
                if (stop_.load())
                    break;
                {
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    auto r = resolved_.find(fn);
                    if (r != resolved_.end() && cache_.count(r->second)) {
                        ++ready;
                        continue;
                    }
                }
                cached c;
                std::lock_guard<std::mutex> io(io_mutex_);
                if (decode_into(fn, c))
                    ++ready;
            }
            decoded_ahead_.store(ready);
            {
                std::lock_guard<std::mutex> io(io_mutex_);
                bad_records_.store(src_->bad_records());
            }

            // 4. Evict what no longer matters: keep the wanted window and the frame on air.
            {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                std::set<int64_t> keep;
                for (const int64_t fn : wanted) {
                    auto r = resolved_.find(fn);
                    if (r != resolved_.end())
                        keep.insert(r->second);
                }
                keep.insert(on_air_fn_.load());
                for (auto it = resolved_.begin(); it != resolved_.end();)
                    it = keep.count(it->second) ? std::next(it) : resolved_.erase(it);
                for (auto it = cache_.begin(); it != cache_.end() && cache_.size() > kCacheCap;)
                    it = keep.count(it->first) ? std::next(it) : cache_.erase(it);
                for (auto it = cache_.begin(); it != cache_.end();)
                    it = keep.count(it->first) || cache_.size() <= kCacheCap / 2 ? std::next(it) : cache_.erase(it);
            }
        } catch (const std::exception&) {
            CASPAR_LOG_CURRENT_EXCEPTION();
        }
    }
}

core::draw_frame replay_producer::receive_impl(core::video_field /*field*/, int /*nb_samples*/)
{
    int64_t show   = INT64_MIN;
    bool    at_end = false;
    double  speed  = 1.0;
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        if (!t_.started) {
            t_.next_fn = static_cast<double>(t_.in_fn == INT64_MIN ? t_.first_fn : t_.in_fn);
            t_.started = true;
        }
        show  = step(t_, at_end);
        speed = t_.speed;
    }
    kick();

    if (show == INT64_MIN)
        return have_last_ ? core::draw_frame::still(last_.frame) : core::draw_frame{};

    cached entry;
    bool   found = false;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto r = resolved_.find(show);
        if (r != resolved_.end()) {
            auto it = cache_.find(r->second);
            if (it != cache_.end()) {
                entry = it->second;
                found = true;
            }
        }
    }
    if (!found) {
        // A MISS: the reader had not reached this frame yet (a seek, a jump). Exactness wins over
        // the channel thread's budget -- the frame asked for is the frame shown -- and every one
        // is counted, so a steady miss rate is a number rather than a guess.
        late_decodes_.fetch_add(1);
        graph_->set_tag(diagnostics::tag_severity::INFO, "late-decode");
        std::lock_guard<std::mutex> io(io_mutex_);
        found = decode_into(show, entry);
    }
    if (!found)
        return have_last_ ? core::draw_frame::still(last_.frame) : core::draw_frame{};

    // AUDIO only at exactly 1x, once per record, and not when holding at an end. Varispeed audio
    // is phase 2 (W16); until then any other speed is SILENT rather than the v1 stutter of each
    // frame's own samples played at the wrong rate.
    const bool play_audio = std::abs(speed - 1.0) < 1e-6 && !at_end && entry.frame_number != last_audio_fn_;
    if (play_audio)
        last_audio_fn_ = entry.frame_number;

    last_      = entry;
    have_last_ = true;
    on_air_fn_.store(entry.frame_number);
    publish(entry, at_end);
    return play_audio ? entry.frame : core::draw_frame::still(entry.frame);
}

void replay_producer::publish(const cached& shown, bool at_end)
{
    transport t;
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        t = t_;
    }
    const int64_t position = std::max<int64_t>(0, shown.serial - t.retired);
    shown_serial_          = shown.serial;
    const double tps       = fmt_.ticks_per_second();

    std::lock_guard<std::mutex> lock(state_mutex_);
    state_["file/frame"]            = position;
    state_["file/length"]           = t.count;
    state_["file/fps"]              = tps;
    state_["file/time"]             = tps > 0 ? static_cast<double>(position) / tps : 0.0;
    state_["replay/frame_number"]   = shown.frame_number;
    state_["replay/live"]           = t.live;
    state_["replay/live_distance"]  = t.live_distance;
    state_["replay/at_end"]         = at_end;
    state_["replay/decoded_ahead"]  = decoded_ahead_.load();
    state_["replay/late_decodes"]   = late_decodes_.load();
    state_["replay/bad_records"]    = bad_records_.load();
    state_["vmx/timestamp"]         = static_cast<int64_t>(shown.wall_us);
    state_["vmx/timestamp_formatted"] = format_wall(shown.wall_us, tps);
}

core::monitor::state replay_producer::state() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

// ── POINTS, SEEKS, LIVE ──────────────────────────────────────────────────────────────────────

int64_t replay_producer::parse_point(const std::wstring& s)
{
    int64_t v = 0;
    if (parse_int(s, v)) {
        if (v < 0)
            refuse(L"a position is a whole number from 0, not '" + s + L"'");
        return v;
    }
    const auto wall = parse_wall(s, fmt_.ticks_per_second());
    if (!wall)
        refuse(L"'" + s + L"' is neither a position nor a time (yyyy-mm-dd-hh-mm-ss-ff or ISO 8601)");
    std::lock_guard<std::mutex> io(io_mutex_);
    return src_->position_of_wall(*wall);
}

void replay_producer::seek_position(int64_t position)
{
    int64_t fn;
    {
        std::lock_guard<std::mutex> io(io_mutex_);
        fn = resolve_position(position);
    }
    {
        std::lock_guard<std::mutex> lock(transport_mutex_);
        t_.next_fn = static_cast<double>(fn);
        t_.started = true;
        t_.live    = false; // any seek leaves follow mode
    }
    last_audio_fn_ = INT64_MIN;
    kick();
}

void replay_producer::set_in(int64_t position)
{
    int64_t fn, retired;
    {
        std::lock_guard<std::mutex> io(io_mutex_);
        fn      = resolve_position(position);
        retired = src_->retired();
    }
    std::lock_guard<std::mutex> lock(transport_mutex_);
    t_.in_fn     = fn;
    t_.in_serial = position + retired;
}

void replay_producer::set_out(int64_t position)
{
    std::lock_guard<std::mutex> io(io_mutex_);
    const int64_t retired = src_->retired();
    const int64_t fn      = position == 0 ? 0 : resolve_position(position);
    std::lock_guard<std::mutex> lock(transport_mutex_);
    t_.out_fn     = fn;
    t_.out_serial = position == 0 ? -1 : position + retired;
}

void replay_producer::enter_live(int64_t distance)
{
    if (distance < 0)
        refuse(L"LIVE takes a distance in frames from 0, not " + std::to_wstring(distance));
    std::lock_guard<std::mutex> lock(transport_mutex_);
    t_.live          = true;
    t_.live_distance = distance;
    t_.speed         = 1.0;
    t_.started       = true;
}

void replay_producer::set_speed(double s)
{
    std::lock_guard<std::mutex> lock(transport_mutex_);
    t_.speed = s;
    if (std::abs(s - 1.0) > 1e-9)
        t_.live = false; // any speed but 1 leaves follow mode
}

std::future<std::wstring> replay_producer::call(const std::vector<std::wstring>& params)
{
    if (params.empty())
        refuse(L"CALL needs a verb");
    const auto& cmd = params.front();
    const auto  arg = [&](size_t i) -> const std::wstring& {
        if (params.size() <= i)
            refuse(L"CALL " + cmd + L" is missing its argument");
        return params[i];
    };

    if (boost::iequals(cmd, L"SEEK")) {
        if (boost::iequals(arg(1), L"LIVE")) {
            int64_t d = 2;
            if (params.size() > 2 && !parse_int(params[2], d))
                refuse(L"SEEK LIVE takes a distance in frames, not '" + params[2] + L"'");
            enter_live(d);
        } else {
            seek_position(parse_point(arg(1)));
        }
        kick();
        return make_ready_future(std::wstring(L"OK"));
    }
    if (boost::iequals(cmd, L"LIVE")) {
        int64_t d = 2;
        if (params.size() > 1 && !parse_int(params[1], d))
            refuse(L"LIVE takes a distance in frames, not '" + params[1] + L"'");
        enter_live(d);
        kick();
        return make_ready_future(std::wstring(L"OK"));
    }
    if (boost::iequals(cmd, L"IN")) {
        set_in(parse_point(arg(1)));
        return make_ready_future(std::wstring(L"OK"));
    }
    if (boost::iequals(cmd, L"OUT")) {
        set_out(parse_point(arg(1)));
        return make_ready_future(std::wstring(L"OK"));
    }
    if (boost::iequals(cmd, L"LOOP")) {
        // A bare LOOP is a QUERY (the cluster watchdog sends exactly that).
        if (params.size() > 1) {
            const auto& v = params[1];
            bool        on;
            if (v == L"1" || boost::iequals(v, L"TRUE") || boost::iequals(v, L"ON"))
                on = true;
            else if (v == L"0" || boost::iequals(v, L"FALSE") || boost::iequals(v, L"OFF"))
                on = false;
            else
                refuse(L"CALL LOOP takes 1 or 0, not '" + v + L"'");
            std::lock_guard<std::mutex> lock(transport_mutex_);
            t_.loop = on;
        }
        std::lock_guard<std::mutex> lock(transport_mutex_);
        return make_ready_future(std::wstring(t_.loop ? L"1" : L"0"));
    }
    if (boost::iequals(cmd, L"SPEED")) {
        double s = 0;
        if (!parse_double(arg(1), s))
            refuse(L"SPEED takes a number, not '" + params[1] + L"'");
        set_speed(s);
        kick();
        return make_ready_future(std::wstring(L"OK"));
    }
    if (boost::iequals(cmd, L"EXPORT"))
        return start_export(params);

    refuse(L"CALL: unknown '" + cmd + L"'. Supported: SEEK, LIVE, IN, OUT, LOOP, SPEED, EXPORT");
}

void replay_producer::configure(const std::vector<std::wstring>& params)
{
    // RECOGNISED keywords with a bad value REFUSE the PLAY (W9); unknown keywords are left for
    // whoever owns them -- the transition (`MIX 25`) travels in this same list.
    for (size_t i = 1; i < params.size(); ++i) {
        const auto p    = boost::to_upper_copy(params[i]);
        const auto next = [&]() -> const std::wstring& {
            if (i + 1 >= params.size())
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(u8(L"[replay] PLAY " + p + L" is missing its value")));
            return params[++i];
        };
        if (p == L"LOOP") {
            std::lock_guard<std::mutex> lock(transport_mutex_);
            t_.loop = true;
        } else if (p == L"IN") {
            set_in(parse_point(next()));
        } else if (p == L"OUT") {
            set_out(parse_point(next()));
        } else if (p == L"LENGTH") {
            int64_t len = 0;
            if (!parse_int(next(), len) || len < 0)
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(u8(L"[replay] PLAY LENGTH takes a frame count, not '" +
                                                                   params[i] + L"'")));
            int64_t in_pos;
            {
                std::lock_guard<std::mutex> lock(transport_mutex_);
                in_pos = t_.in_serial < 0 ? 0 : t_.in_serial - t_.retired;
            }
            set_out(in_pos + len);
        } else if (p == L"SEEK") {
            const auto& v = next();
            if (boost::iequals(v, L"LIVE"))
                enter_live(2);
            else
                seek_position(parse_point(v));
        } else if (p == L"LIVE") {
            int64_t d = 2;
            if (i + 1 < params.size() && parse_int(params[i + 1], d))
                ++i;
            enter_live(d);
        } else if (p == L"SPEED") {
            double s = 0;
            if (!parse_double(next(), s))
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(u8(L"[replay] PLAY SPEED takes a number, not '" +
                                                                   params[i] + L"'")));
            set_speed(s);
        }
    }
    kick();
}

}} // namespace caspar::replay
