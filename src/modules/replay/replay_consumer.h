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
#include "replay_store.h"

#include <common/diagnostics/graph.h>
#include <common/memory.h>
#include <core/consumer/frame_consumer.h>
#include <core/monitor/monitor.h>
#include <core/video_format.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace caspar { namespace replay {

/// What `ADD n REPLAY "<name>?..." [quality]` asked for, parsed and checked -- and NOTHING done
/// about it yet. `REMOVE n REPLAY "<name>"` builds one of these to find the consumer's index, so
/// constructing a consumer must have no side effects at all (W2).
struct record_request
{
    std::wstring name;                  ///< as given, without the `?query`
    int          max_duration_sec = 86400;
    int          segment_sec      = 60;
    uint32_t     vmx_profile      = 0;  ///< VMX_PROFILE; 0 = SQ
};

/// `ADD` options from the query string; throws `user_error` on anything it does not recognise.
record_request parse_record_request(const std::wstring& name_with_query, const std::wstring& quality);

class replay_consumer final : public core::frame_consumer
{
  public:
    explicit replay_consumer(record_request req);
    ~replay_consumer() override;

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override;

    std::future<bool> send(core::video_field field, core::const_frame frame) override;

    std::wstring         print() const override;
    std::wstring         name() const override { return L"replay"; }
    bool                 has_synchronization_clock() const override { return false; }
    int                  index() const override { return index_; }
    core::monitor::state state() const override;

    /// 200000 + FNV-1a of the lower-cased requested name, mod 9999. From the NAME, so two
    /// recordings on one channel do not collide (W1) and REMOVE finds the one it names.
    static int index_for(const std::wstring& name);

  private:
    struct pending
    {
        host_image           image;
        std::vector<int32_t> audio;
        store::record_meta   meta;
    };

    void worker_loop();
    void write_one(pending& p);
    void fail(const std::string& why);
    void publish();

    record_request req_;
    int            index_;

    boost::filesystem::path         dir_;
    store::format                   fmt_;
    std::unique_ptr<encoder>        encoder_;
    std::unique_ptr<store::writer>  writer_;
    std::vector<uint8_t>            encoded_;

    std::deque<pending>     queue_;
    std::mutex              queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread             worker_;
    std::atomic<bool>       stop_{false};
    static constexpr size_t queue_capacity_ = 4;

    int64_t              ticks_ = 0;           ///< phase 1 frame number: this recording's tick count
    std::atomic<int64_t> frames_written_{0};
    std::atomic<int64_t> frames_dropped_{0};
    std::atomic<bool>    failed_{false};
    bool                 drop_warned_          = false;
    bool                 encode_warned_        = false;

    mutable std::mutex   state_mutex_;
    core::monitor::state state_;
    std::string          error_;

    spl::shared_ptr<diagnostics::graph> graph_;
};

}} // namespace caspar::replay
