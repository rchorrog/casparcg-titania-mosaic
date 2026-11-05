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

// THE REPLAY CODECS. One interface, so the store does not care which codec filled a record and
// phase 3 can add the CUDA ProRes codec beside these without touching the consumer or producer.
//
//   vmx-bgra8  8-bit channels. libvmx converts BGRA <-> Y'CbCr itself (BT.601/709 only).
//   vmx-p216   16-bit channels. WE convert the channel's RGB to 10-bit 4:2:2 Y'CbCr with the
//              channel's own matrix -- BT.2020 included, which libvmx's RGB path cannot do -- and
//              libvmx codes the planes with its 16-bit plane coder. Decode hands the mixer three
//              16-bit Y'CbCr planes and the matrix, and the mixer converts, as it does for every
//              other Y'CbCr source.

#pragma once

#include "replay_store.h"

#include <core/frame/frame.h>
#include <core/frame/frame_factory.h>
#include <core/frame/pixel_format.h>

#include <memory>
#include <optional>
#include <vector>

namespace caspar { namespace replay {

/// A copy of one channel frame's host image, made on the channel thread and encoded on the worker.
struct host_image
{
    std::vector<uint8_t> bytes;
    int                  stride   = 0;     ///< bytes per row
    bool                 is_16bit = false; ///< 8 bytes per pixel instead of 4
    bool                 rgba     = false; ///< R first in memory (the Vulkan mixer at 16 bits) instead of B
};

class encoder
{
  public:
    virtual ~encoder() = default;
    /// Encodes `img` into `out` (replaced). FALSE when the codec rejected the frame.
    virtual bool encode(const host_image& img, std::vector<uint8_t>& out) = 0;
};

class decoder
{
  public:
    virtual ~decoder() = default;
    /// Decodes `video` into a new frame from `factory`; empty when the codec rejected the payload.
    virtual std::optional<core::mutable_frame>
    decode(const std::vector<uint8_t>& video, core::frame_factory& factory, const void* tag) = 0;
};

/// The codec a channel of this depth records with.
store::codec codec_for_depth(bool is_16bit);

std::unique_ptr<encoder> make_encoder(const store::format& fmt);
std::unique_ptr<decoder> make_decoder(const store::format& fmt);

/// The Y'CbCr matrix the MIXER will decode a `color_space` tag with -- BT.601, BT.709 or BT.2020,
/// everything else falling back to BT.709 exactly as `image_kernel` does. The 10-bit encoder uses
/// this and nothing else, so encode and decode cannot disagree about a space.
void luma_coefficients(core::color_space cs, double& kr, double& kb);

}} // namespace caspar::replay
