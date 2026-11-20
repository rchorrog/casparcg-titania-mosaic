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

#include "replay_codec.h"

#include "vmxcodec.h"

#include <common/except.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace caspar { namespace replay {

void luma_coefficients(core::color_space cs, double& kr, double& kb)
{
    // ITU-R BT.601-7, BT.709-6, BT.2020-2. The fallback is image_kernel.cpp's
    // `cs_idx = color_space > 2 ? 1 : color_space`, kept identical on purpose.
    switch (cs) {
        case core::color_space::bt601:
            kr = 0.299;
            kb = 0.114;
            return;
        case core::color_space::bt2020:
            kr = 0.2627;
            kb = 0.0593;
            return;
        default:
            kr = 0.2126;
            kb = 0.0722;
            return;
    }
}

store::codec codec_for_depth(bool is_16bit) { return is_16bit ? store::codec::vmx_p216 : store::codec::vmx_bgra8; }

namespace {

VMX_INSTANCE* create_vmx(const store::format& fmt)
{
    VMX_SIZE dim = {fmt.width, fmt.height};
    const auto profile =
        fmt.vmx_profile ? static_cast<VMX_PROFILE>(fmt.vmx_profile) : VMX_PROFILE_SQ;
    // The colour space argument matters only to libvmx's own RGB conversion, i.e. to vmx-bgra8.
    const auto cs = static_cast<core::color_space>(fmt.color_space) == core::color_space::bt601 ? VMX_COLORSPACE_BT601
                                                                                                 : VMX_COLORSPACE_BT709;
    return VMX_Create(dim, profile, cs);
}

int save(VMX_INSTANCE* vmx, std::vector<uint8_t>& out, size_t cap)
{
    out.resize(cap);
    const int n = VMX_SaveTo(vmx, out.data(), static_cast<int>(cap));
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return n;
}

// ── vmx-bgra8 ─────────────────────────────────────────────────────────────────────────────────

class vmx_bgra8_encoder final : public encoder
{
    VMX_INSTANCE*  vmx_;
    store::format  fmt_;

  public:
    explicit vmx_bgra8_encoder(const store::format& fmt)
        : vmx_(create_vmx(fmt))
        , fmt_(fmt)
    {
        if (!vmx_)
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("replay: VMX_Create failed"));
    }
    ~vmx_bgra8_encoder() override { VMX_Destroy(vmx_); }

    bool encode(const host_image& img, std::vector<uint8_t>& out) override
    {
        if (img.is_16bit || img.stride < fmt_.width * 4 ||
            img.bytes.size() < static_cast<size_t>(img.stride) * fmt_.height)
            return false;
        if (VMX_EncodeBGRA(vmx_, const_cast<BYTE*>(img.bytes.data()), img.stride, 0) != VMX_ERR_OK)
            return false;
        return save(vmx_, out, static_cast<size_t>(fmt_.width) * fmt_.height * 4) > 0;
    }
};

class vmx_bgra8_decoder final : public decoder
{
    VMX_INSTANCE* vmx_;
    store::format fmt_;

  public:
    explicit vmx_bgra8_decoder(const store::format& fmt)
        : vmx_(create_vmx(fmt))
        , fmt_(fmt)
    {
        if (!vmx_)
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("replay: VMX_Create failed"));
    }
    ~vmx_bgra8_decoder() override { VMX_Destroy(vmx_); }

    std::optional<core::mutable_frame>
    decode(const std::vector<uint8_t>& video, core::frame_factory& factory, const void* tag) override
    {
        if (video.empty() ||
            VMX_LoadFrom(vmx_, const_cast<BYTE*>(video.data()), static_cast<int>(video.size())) != VMX_ERR_OK)
            return std::nullopt;
        core::pixel_format_desc desc(core::pixel_format::bgra, static_cast<core::color_space>(fmt_.color_space));
        desc.planes.emplace_back(fmt_.width, fmt_.height, 4);
        auto frame = factory.create_frame(tag, desc);
        if (VMX_DecodeBGRA(vmx_, frame.image_data(0).data(), fmt_.width * 4) != VMX_ERR_OK)
            return std::nullopt;
        return std::optional<core::mutable_frame>(std::move(frame));
    }
};

// ── vmx-p216: 10-bit 4:2:2 ──────────────────────────────────────────────────────────────────
//
// P216 is a 16-bit Y plane followed by a 16-bit interleaved CbCr plane at half horizontal
// resolution, 10 significant bits MSB-aligned (libvmx: "the 6 least significant bits will be 0").
// Studio range: Y' 64..940, C 64..960, the BT.709/2020 10-bit code points.
//
// CHROMA IS CO-SITED with the even luma sample, by a [1 2 1]/4 filter centred on it -- the 4:2:2
// siting BT.709 and BT.2020 specify. Averaging pairs would site it BETWEEN the samples instead.
// CasparCG 2.5.0's mixer has no chroma-siting tag and reconstructs chroma as centre-sited, so on
// this tree 10-bit playback carries a half-luma-pixel horizontal chroma offset. Only channels
// with color-depth 16 use this codec.

class vmx_p216_encoder final : public encoder
{
    VMX_INSTANCE*         vmx_;
    store::format         fmt_;
    double                kr_ = 0.2126, kb_ = 0.0722;
    std::vector<uint16_t> p216_;

  public:
    explicit vmx_p216_encoder(const store::format& fmt)
        : vmx_(create_vmx(fmt))
        , fmt_(fmt)
    {
        if (!vmx_)
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("replay: VMX_Create failed"));
        luma_coefficients(static_cast<core::color_space>(fmt.color_space), kr_, kb_);
        p216_.resize(static_cast<size_t>(fmt.width) * fmt.height * 2);
    }
    ~vmx_p216_encoder() override { VMX_Destroy(vmx_); }

    bool encode(const host_image& img, std::vector<uint8_t>& out) override
    {
        const int w = fmt_.width, h = fmt_.height;
        if (!img.is_16bit || img.stride < w * 8 || img.bytes.size() < static_cast<size_t>(img.stride) * h)
            return false;

        const double kg = 1.0 - kr_ - kb_;
        const double cb_scale = 1.0 / (2.0 * (1.0 - kb_));
        const double cr_scale = 1.0 / (2.0 * (1.0 - kr_));
        // Channel samples are 0..65535 for 0..1; studio-range 10-bit, then <<6 to MSB-align.
        const int r_off = img.rgba ? 0 : 2;
        const int b_off = img.rgba ? 2 : 0;

        uint16_t* y_plane  = p216_.data();
        uint16_t* uv_plane = p216_.data() + static_cast<size_t>(w) * h;

        tbb::parallel_for(0, h, [&](int row) {
            const auto* src = reinterpret_cast<const uint16_t*>(img.bytes.data() + static_cast<size_t>(row) * img.stride);
            uint16_t*   yp  = y_plane + static_cast<size_t>(row) * w;
            uint16_t*   cp  = uv_plane + static_cast<size_t>(row) * w;

            const auto sample = [&](int x, double& r, double& g, double& b) {
                x            = std::clamp(x, 0, w - 1);
                const auto* p = src + static_cast<size_t>(x) * 4;
                r             = p[r_off] / 65535.0;
                g             = p[1] / 65535.0;
                b             = p[b_off] / 65535.0;
            };
            const auto q = [](double v, double lo, double hi) {
                return static_cast<uint16_t>(static_cast<int>(std::lround(std::clamp(v, lo, hi))) << 6);
            };

            for (int x = 0; x < w; ++x) {
                double r, g, b;
                sample(x, r, g, b);
                yp[x] = q(64.0 + 876.0 * (kr_ * r + kg * g + kb_ * b), 4.0, 1019.0);
            }
            for (int x = 0; x < w; x += 2) {
                double cb = 0, cr = 0;
                const double taps[3] = {0.25, 0.5, 0.25};
                for (int k = -1; k <= 1; ++k) {
                    double r, g, b;
                    sample(x + k, r, g, b);
                    const double yv = kr_ * r + kg * g + kb_ * b;
                    cb += taps[k + 1] * (b - yv) * cb_scale;
                    cr += taps[k + 1] * (r - yv) * cr_scale;
                }
                cp[x]     = q(512.0 + 896.0 * cb, 4.0, 1019.0);
                cp[x + 1] = q(512.0 + 896.0 * cr, 4.0, 1019.0);
            }
        });

        if (VMX_EncodeP216(vmx_, reinterpret_cast<BYTE*>(p216_.data()), w * 2, 0) != VMX_ERR_OK)
            return false;
        return save(vmx_, out, static_cast<size_t>(w) * h * 4) > 0;
    }
};

class vmx_p216_decoder final : public decoder
{
    VMX_INSTANCE*         vmx_;
    store::format         fmt_;
    std::vector<uint16_t> p216_;

  public:
    explicit vmx_p216_decoder(const store::format& fmt)
        : vmx_(create_vmx(fmt))
        , fmt_(fmt)
    {
        if (!vmx_)
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("replay: VMX_Create failed"));
        p216_.resize(static_cast<size_t>(fmt.width) * fmt.height * 2);
    }
    ~vmx_p216_decoder() override { VMX_Destroy(vmx_); }

    std::optional<core::mutable_frame>
    decode(const std::vector<uint8_t>& video, core::frame_factory& factory, const void* tag) override
    {
        const int w = fmt_.width, h = fmt_.height;
        if (video.empty() ||
            VMX_LoadFrom(vmx_, const_cast<BYTE*>(video.data()), static_cast<int>(video.size())) != VMX_ERR_OK)
            return std::nullopt;
        if (VMX_DecodeP216(vmx_, reinterpret_cast<BYTE*>(p216_.data()), w * 2) != VMX_ERR_OK)
            return std::nullopt;

        // Three planes for the mixer, `bit16` because the values are MSB-aligned (the rule
        // av_util applies to P010: bit10 would mean LSB-aligned and scale by 64).
        // CasparCG 2.5.0's pixel_format_desc carries no transfer, range or chroma siting: the
        // stock mixer always decodes YCbCr as studio range with centre-sited chroma.
        core::pixel_format_desc desc(core::pixel_format::ycbcr, static_cast<core::color_space>(fmt_.color_space));
        desc.planes.emplace_back(w, h, 1, common::bit_depth::bit16);
        desc.planes.emplace_back(w / 2, h, 1, common::bit_depth::bit16);
        desc.planes.emplace_back(w / 2, h, 1, common::bit_depth::bit16);
        auto frame = factory.create_frame(tag, desc);

        const uint16_t* y_src  = p216_.data();
        const uint16_t* uv_src = p216_.data() + static_cast<size_t>(w) * h;
        // CasparCG 2.5.0's ycbcra_to_rgba() does `sample * 255 - (16, 128, 128)`, i.e. it counts a
        // 16-bit texture in 8-bit codes: an MSB-aligned 10-bit code c arrives as c*64/65535, and
        // c*64/65535*255 is c/4 - c/1028, so neutral chroma (512) would land on 127.5 rather than
        // 128 -- a constant green cast. Rescaling by 65535/65280 makes the sample c/4 exactly in
        // that arithmetic (legal black 64 -> 16, neutral 512 -> 128, white 940 -> 235).
        const auto to_stock = [](uint16_t v) -> uint16_t {
            const uint32_t r = (static_cast<uint32_t>(v) * 65535u + 32640u) / 65280u;
            return static_cast<uint16_t>(r > 65535u ? 65535u : r);
        };
        auto* y  = reinterpret_cast<uint16_t*>(frame.image_data(0).data());
        auto* cb = reinterpret_cast<uint16_t*>(frame.image_data(1).data());
        auto* cr = reinterpret_cast<uint16_t*>(frame.image_data(2).data());
        tbb::parallel_for(0, h, [&](int row) {
            const uint16_t* ys = y_src + static_cast<size_t>(row) * w;
            uint16_t*       yd = y + static_cast<size_t>(row) * w;
            for (int x = 0; x < w; ++x)
                yd[x] = to_stock(ys[x]);
            const uint16_t* s  = uv_src + static_cast<size_t>(row) * w;
            uint16_t*       db = cb + static_cast<size_t>(row) * (w / 2);
            uint16_t*       dr = cr + static_cast<size_t>(row) * (w / 2);
            for (int x = 0; x < w / 2; ++x) {
                db[x] = to_stock(s[2 * x]);
                dr[x] = to_stock(s[2 * x + 1]);
            }
        });
        return std::optional<core::mutable_frame>(std::move(frame));
    }
};

} // namespace

std::unique_ptr<encoder> make_encoder(const store::format& fmt)
{
    switch (fmt.video_codec) {
        case store::codec::vmx_bgra8:
            return std::make_unique<vmx_bgra8_encoder>(fmt);
        case store::codec::vmx_p216:
            return std::make_unique<vmx_p216_encoder>(fmt);
        default:
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(std::string("replay: no encoder for ") +
                                                                  store::codec_name(fmt.video_codec)));
    }
}

std::unique_ptr<decoder> make_decoder(const store::format& fmt)
{
    switch (fmt.video_codec) {
        case store::codec::vmx_bgra8:
            return std::make_unique<vmx_bgra8_decoder>(fmt);
        case store::codec::vmx_p216:
            return std::make_unique<vmx_p216_decoder>(fmt);
        default:
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info(std::string("replay: no decoder for ") +
                                                                  store::codec_name(fmt.video_codec)));
    }
}

}} // namespace caspar::replay
