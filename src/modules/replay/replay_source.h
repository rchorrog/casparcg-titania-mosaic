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

// What the producer and the exporter read a recording through: the v2 store, or -- read-only, so
// recordings made before replay v2 still play -- the v1 `.mav`/`.idx` layout.

#pragma once

#include "replay_store.h"

#include <boost/filesystem.hpp>

#include <memory>
#include <vector>

namespace caspar { namespace replay {

class source
{
  public:
    virtual ~source() = default;

    virtual const store::format& fmt() const = 0;
    virtual void                 refresh()   = 0;
    virtual int64_t              count() const = 0;
    virtual bool    read(int64_t position, store::record_meta& meta, std::vector<uint8_t>& audio,
                         std::vector<uint8_t>& video)                       = 0;
    virtual bool    meta_at(int64_t position, store::record_meta& meta) const = 0;
    virtual int64_t position_of_frame(int64_t frame_number) const           = 0;
    virtual int64_t position_of_wall(uint64_t wall_us) const                = 0;
    virtual int64_t bad_records() const                                     = 0;
    virtual int64_t retired() const                                         = 0;
    virtual bool    is_legacy() const                                       = 0;
};

/// Opens the recording `name` (a media-relative name or an absolute path). Null when there is no
/// recording there in either layout.
std::unique_ptr<source> open_source(const std::wstring& name);

/// True when `name` names a recording in either layout -- the producer factory's probe.
bool is_recording(const std::wstring& name);

}} // namespace caspar::replay
