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

#pragma once

#include "replay_codec.h"
#include "replay_source.h"

#include <common/diagnostics/graph.h>
#include <core/frame/draw_frame.h>
#include <core/frame/frame_factory.h>
#include <core/producer/frame_producer.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace caspar { namespace replay {

/// Plays a recording -- finished, growing, or rolling under retention.
///
/// THE CHANNEL THREAD NEVER READS OR DECODES (W5). A reader thread decodes ahead of the playhead
/// in the direction of play, predicting the next ticks with the same arithmetic `receive_impl`
/// uses; `receive_impl` picks the frame for this tick out of that cache. Only a miss -- a seek the
/// reader has not caught up with -- decodes on the channel thread, and each one is counted in
/// `replay/late_decodes`, which steady playback keeps at zero.
///
/// THE PLAYHEAD IS A FRAME NUMBER, not a position: positions are renumbered every time retention
/// drops a segment from the front, frame numbers never are. `position`, `in` and `out` are
/// positions on the wire and are converted at the boundary.
class replay_producer final : public core::frame_producer
{
  public:
    replay_producer(std::unique_ptr<source> src, std::wstring display_name,
                    spl::shared_ptr<core::frame_factory> frame_factory);
    ~replay_producer() override;

    core::draw_frame          receive_impl(core::video_field field, int nb_samples) override;
    std::future<std::wstring> call(const std::vector<std::wstring>& params) override;
    bool                      is_ready() override { return true; }

    /// The PLAY keywords. Throws `user_error` on an unknown keyword or a value that does not parse (W9).
    void configure(const std::vector<std::wstring>& params);

    std::wstring         print() const override { return L"replay[" + display_name_ + L"]"; }
    std::wstring         name() const override { return L"replay"; }
    core::monitor::state state() const override;

  private:
    struct cached
    {
        core::draw_frame frame;      ///< carries the record's audio; `draw_frame::still` mutes it
        int64_t          serial       = 0; ///< position + retired() when decoded: stable under retention
        int64_t          frame_number = 0;
        uint64_t         wall_us      = 0;
    };

    /// Everything the transport needs, under `transport_mutex_`.
    struct transport
    {
        double  next_fn      = 0;   ///< frame number to show on the NEXT tick (fractional at speeds != 1)
        bool    started      = false;
        double  speed        = 1.0;
        bool    loop         = false;
        int64_t in_fn        = INT64_MIN; ///< INT64_MIN = the oldest record held
        int64_t out_fn       = 0;         ///< EXCLUSIVE end; 0 = follow the material
        int64_t in_serial    = -1;        ///< what `in` reports; -1 = 0
        int64_t out_serial   = -1;        ///< what `out` reports; -1 = follow
        bool    live         = false;
        int64_t live_distance = 2;

        // A snapshot the reader thread maintains.
        int64_t first_fn = 0, last_fn = -1, count = 0, retired = 0;
    };

    /// The frame number shown on a tick with `t`, and `t` advanced past it. Shared by the channel
    /// thread (for real) and the reader thread (to predict), so the prediction cannot drift.
    static int64_t step(transport& t, bool& at_end);

    void reader_loop();
    bool decode_into(int64_t fn, cached& out); ///< under io_mutex_
    /// position -> frame number; `count` maps to one past the last. Under io_mutex_.
    int64_t resolve_position(int64_t position);
    /// A CALL/PLAY point: a position, or a timestamp, resolved to a POSITION. Throws on garbage.
    int64_t parse_point(const std::wstring& s);
    void    seek_position(int64_t position);
    void    set_in(int64_t position);
    void    set_out(int64_t position);
    void    enter_live(int64_t distance);
    void    set_speed(double s);
    void    publish(const cached& shown, bool at_end);
    void    kick();

    std::wstring                         display_name_;
    spl::shared_ptr<core::frame_factory> frame_factory_;
    spl::shared_ptr<diagnostics::graph>  graph_;

    // io: the source and the decoder, used by the reader thread and by a channel-thread miss.
    std::mutex               io_mutex_;
    std::unique_ptr<source>  src_;
    std::unique_ptr<decoder> decoder_;
    store::format            fmt_;

    std::mutex transport_mutex_;
    transport  t_;

    std::mutex                 cache_mutex_;
    std::map<int64_t, cached>  cache_;      ///< keyed by RECORD frame number
    std::map<int64_t, int64_t> resolved_;   ///< display frame number -> record frame number

    std::thread             reader_;
    std::condition_variable reader_cv_;
    std::mutex              reader_cv_mutex_;
    std::atomic<bool>       stop_{false};
    std::atomic<bool>       kick_{false};

    cached               last_;
    bool                 have_last_ = false;
    int64_t              last_audio_fn_ = INT64_MIN;
    std::atomic<int64_t> shown_serial_{0};
    std::atomic<int64_t> on_air_fn_{INT64_MIN}; ///< what the reader must not evict
    std::atomic<int64_t> bad_records_{0};       ///< the source's count, copied under io_mutex_
    std::atomic<int64_t> late_decodes_{0};
    std::atomic<int64_t> decoded_ahead_{0};

    mutable std::mutex   state_mutex_;
    core::monitor::state state_;

    // EXPORT (replay_export.cpp)
    std::thread       export_thread_;
    std::atomic<bool> export_running_{false};
    std::atomic<int64_t> export_frames_{0};
    std::future<std::wstring> start_export(const std::vector<std::wstring>& params);
};

}} // namespace caspar::replay
