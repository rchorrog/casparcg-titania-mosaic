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
 */

#include "replay.h"

#include "replay_consumer.h"
#include "replay_producer.h"
#include "replay_source.h"

#include <common/except.h>
#include <common/utf.h>
#include <core/consumer/frame_consumer_registry.h>
#include <core/module_dependencies.h>
#include <core/producer/frame_producer_registry.h>

#include <boost/algorithm/string.hpp>

namespace caspar { namespace replay {

namespace {

spl::shared_ptr<core::frame_producer> create_producer(const core::frame_producer_dependencies& dependencies,
                                                      const std::vector<std::wstring>&         params)
{
    if (params.empty())
        return core::frame_producer::empty();

    // `name`, `name.mav` (the v1 spelling), or an absolute path to a recording directory -- in
    // either store layout. Matched on what is ON DISK, so a clip and a recording with different
    // names never compete for a PLAY.
    std::wstring name = params[0];
    if (boost::iends_with(name, L".mav"))
        name.resize(name.size() - 4);
    if (!is_recording(name))
        return core::frame_producer::empty();

    auto src = open_source(name);
    if (!src)
        return core::frame_producer::empty();

    auto p = spl::make_shared<replay_producer>(std::move(src), name, dependencies.frame_factory);
    if (params.size() > 1)
        p->configure(params);
    return p;
}

spl::shared_ptr<core::frame_consumer> create_consumer(const std::vector<std::wstring>& params,
                                                      const core::video_format_repository& /*format_repository*/,
                                                      const std::vector<spl::shared_ptr<core::video_channel>>& /*channels*/,
                                                      const core::channel_info& /*channel_info*/)
{
    // ADD n REPLAY "<name>[?options]" [quality]
    if (params.size() < 2 || !boost::iequals(params.at(0), L"REPLAY"))
        return core::frame_consumer::empty();
    // Parsing only. The consumer touches the disk in `initialize`, so REMOVE -- which builds a
    // consumer from its own parameters to find the index -- creates and renames nothing.
    return spl::make_shared<replay_consumer>(parse_record_request(params[1], params.size() > 2 ? params[2] : L""));
}

} // namespace

void init(const core::module_dependencies& dependencies)
{
    dependencies.producer_registry->register_producer_factory(L"Replay Producer", create_producer);
    dependencies.consumer_registry->register_consumer_factory(L"Replay Consumer", create_consumer);
}

void uninit() {}

}} // namespace caspar::replay
