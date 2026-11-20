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

// EXPORT, IN PROCESS (W12).
//
// v1 piped raw BGRA into `ffmpeg` found on PATH: libx264 only, NO AUDIO, `-r (int)fps` -- so
// 59.94 was written as 59 and 29.97 as 29 -- the exit code ignored, and "Export Process Complete"
// logged whatever had happened, to nobody but the log. Here the recording is decoded on the CPU
// (its own VMX instance, never the mixer's frame factory) and encoded with libav:
//
//   video  ProRes 422 HQ (`prores_ks`, profile 3), yuv422p10le, studio range, the recording's
//          colour tags, at the recording's EXACT rational record rate
//   audio  every recorded channel as PCM s24le
//   result published on the PRODUCER: export/state, export/path, export/frames, export/error
//
// Phase 3 replaces the re-encode with a REMUX for ProRes recordings.

#include "replay_producer.h"
#include "replay_source.h"

#include "vmxcodec.h"

#include <common/env.h>
#include <common/except.h>
#include <common/future.h>
#include <common/log.h>
#include <common/utf.h>

#include <boost/algorithm/string.hpp>

#if defined(_MSC_VER)
#pragma warning(push, 1)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// FFMPEG_NEW_CHANNEL_LAYOUT: the same switch the ffmpeg module uses, so this builds on FFmpeg 4.4
// (Ubuntu 22.04, which CI and the .deb build) as well as on 6.x/7.x.
#include <modules/ffmpeg/defines.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <thread>
#include <cstring>
#include <functional>
#include <memory>

namespace caspar { namespace replay {

namespace {

struct job
{
    std::wstring input;
    int64_t      in  = 0;  ///< position
    int64_t      out = -1; ///< exclusive position; -1 = to the end
};

std::string av_err(int e)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

AVColorSpace av_space(core::color_space cs)
{
    switch (cs) {
        case core::color_space::bt601:
            return AVCOL_SPC_SMPTE170M;
        case core::color_space::bt2020:
            return AVCOL_SPC_BT2020_NCL;
        default:
            return AVCOL_SPC_BT709;
    }
}
AVColorPrimaries av_prim(core::color_space cs)
{
    switch (cs) {
        case core::color_space::bt601:
            return AVCOL_PRI_SMPTE170M;
        case core::color_space::bt2020:
            return AVCOL_PRI_BT2020;
        default:
            return AVCOL_PRI_BT709;
    }
}

/// Decodes records to yuv422p10le on the CPU.
class cpu_decoder
{
    VMX_INSTANCE*         vmx_ = nullptr;
    store::format         fmt_;
    SwsContext*           sws_ = nullptr;
    std::vector<uint8_t>  bgra_;
    std::vector<uint16_t> p216_;

  public:
    explicit cpu_decoder(const store::format& f)
        : fmt_(f)
    {
        VMX_SIZE dim = {f.width, f.height};
        vmx_         = VMX_Create(dim, f.vmx_profile ? static_cast<VMX_PROFILE>(f.vmx_profile) : VMX_PROFILE_SQ,
                          static_cast<core::color_space>(f.color_space) == core::color_space::bt601 ? VMX_COLORSPACE_BT601
                                                                                                   : VMX_COLORSPACE_BT709);
        if (!vmx_)
            CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("export: VMX_Create failed"));
        if (f.video_codec == store::codec::vmx_bgra8) {
            bgra_.resize(static_cast<size_t>(f.width) * f.height * 4);
            sws_ = sws_getContext(f.width, f.height, AV_PIX_FMT_BGRA, f.width, f.height, AV_PIX_FMT_YUV422P10LE,
                                  SWS_BICUBIC | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
            // Full-range RGB in, studio-range Y'CbCr out, with the recording's matrix.
            const int* coefs = sws_getCoefficients(
                static_cast<core::color_space>(f.color_space) == core::color_space::bt601 ? SWS_CS_ITU601 : SWS_CS_ITU709);
            sws_setColorspaceDetails(sws_, coefs, 1, coefs, 0, 0, 1 << 16, 1 << 16);
        } else {
            p216_.resize(static_cast<size_t>(f.width) * f.height * 2);
        }
    }
    ~cpu_decoder()
    {
        if (sws_)
            sws_freeContext(sws_);
        if (vmx_)
            VMX_Destroy(vmx_);
    }

    bool decode(const std::vector<uint8_t>& video, AVFrame* dst)
    {
        const int w = fmt_.width, h = fmt_.height;
        if (VMX_LoadFrom(vmx_, const_cast<BYTE*>(video.data()), static_cast<int>(video.size())) != VMX_ERR_OK)
            return false;
        if (fmt_.video_codec == store::codec::vmx_bgra8) {
            if (VMX_DecodeBGRA(vmx_, bgra_.data(), w * 4) != VMX_ERR_OK)
                return false;
            const uint8_t* src[1]    = {bgra_.data()};
            const int      stride[1] = {w * 4};
            sws_scale(sws_, src, stride, 0, h, dst->data, dst->linesize);
            return true;
        }
        if (VMX_DecodeP216(vmx_, reinterpret_cast<BYTE*>(p216_.data()), w * 2) != VMX_ERR_OK)
            return false;
        // P216 (MSB-aligned) -> yuv422p10le (LSB-aligned, planar).
        const uint16_t* ys = p216_.data();
        const uint16_t* cs = p216_.data() + static_cast<size_t>(w) * h;
        for (int y = 0; y < h; ++y) {
            auto* yd = reinterpret_cast<uint16_t*>(dst->data[0] + y * dst->linesize[0]);
            auto* ud = reinterpret_cast<uint16_t*>(dst->data[1] + y * dst->linesize[1]);
            auto* vd = reinterpret_cast<uint16_t*>(dst->data[2] + y * dst->linesize[2]);
            for (int x = 0; x < w; ++x)
                yd[x] = static_cast<uint16_t>(ys[static_cast<size_t>(y) * w + x] >> 6);
            for (int x = 0; x < w / 2; ++x) {
                ud[x] = static_cast<uint16_t>(cs[static_cast<size_t>(y) * w + 2 * x] >> 6);
                vd[x] = static_cast<uint16_t>(cs[static_cast<size_t>(y) * w + 2 * x + 1] >> 6);
            }
        }
        return true;
    }
};

using publish_fn = std::function<void(const char* state, int64_t frames, const std::string& error)>;

/// Runs the jobs into `out`. Empty string on success, else the reason.
std::string run_export(const std::vector<job>& jobs, const boost::filesystem::path& out, const publish_fn& publish)
{
    if (jobs.empty())
        return "no input to export";

    auto first = open_source(jobs.front().input);
    if (!first)
        return "cannot open recording " + u8(jobs.front().input);
    const store::format fmt = first->fmt();
    first.reset();

    // ── output ──
    AVFormatContext* oc  = nullptr;
    const auto       ext = boost::to_lower_copy(out.extension().string());
    const char*      fmt_name = ext == ".mxf" ? "mxf" : ext == ".mkv" ? "matroska" : "mov";
    int              e   = avformat_alloc_output_context2(&oc, nullptr, fmt_name, out.string().c_str());
    if (e < 0 || !oc)
        return "cannot create output: " + av_err(e);
    std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> oc_guard(oc, [](AVFormatContext* c) {
        if (c->pb)
            avio_closep(&c->pb);
        avformat_free_context(c);
    });

    // The record rate: frames per second on a progressive channel, FIELDS per second on an
    // interlaced one, because the recorder writes one record per tick.
    const AVRational rate = av_make_q(fmt.rate_num * fmt.field_count, fmt.rate_den);

    // ── video ──
    const AVCodec* vcodec = avcodec_find_encoder_by_name("prores_ks");
    if (!vcodec)
        return "this FFmpeg has no prores_ks encoder";
    AVCodecContext* vc = avcodec_alloc_context3(vcodec);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> vc_guard(vc, [](AVCodecContext* c) {
        avcodec_free_context(&c);
    });
    vc->width           = fmt.width;
    vc->height          = fmt.height;
    vc->pix_fmt         = AV_PIX_FMT_YUV422P10LE;
    vc->time_base       = av_inv_q(rate);
    vc->framerate       = rate;
    vc->color_range     = AVCOL_RANGE_MPEG;
    vc->colorspace      = av_space(static_cast<core::color_space>(fmt.color_space));
    vc->color_primaries = av_prim(static_cast<core::color_space>(fmt.color_space));
    vc->color_trc       = static_cast<core::color_space>(fmt.color_space) == core::color_space::bt2020 ? AVCOL_TRC_BT2020_10
                                                                                                     : AVCOL_TRC_BT709;
    av_opt_set_int(vc->priv_data, "profile", 3, 0); // 422 HQ
    // Frame threads, capped at half the machine: an export is background work beside a live channel.
    vc->thread_type  = FF_THREAD_FRAME;
    vc->thread_count = std::max(1, std::min(8, static_cast<int>(std::thread::hardware_concurrency()) / 2));
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((e = avcodec_open2(vc, vcodec, nullptr)) < 0)
        return "prores_ks would not open: " + av_err(e);
    AVStream* vs = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(vs->codecpar, vc);
    vs->time_base       = vc->time_base;
    vs->avg_frame_rate  = rate;
    vs->r_frame_rate    = rate;

    // ── audio ──
    AVCodecContext* ac = nullptr;
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> ac_guard(nullptr, [](AVCodecContext* c) {
        avcodec_free_context(&c);
    });
    AVStream* as = nullptr;
    if (fmt.audio_channels > 0) {
        const AVCodec* acodec = avcodec_find_encoder(AV_CODEC_ID_PCM_S24LE);
        ac                    = avcodec_alloc_context3(acodec);
        ac_guard.reset(ac);
        ac->sample_fmt  = AV_SAMPLE_FMT_S32; // the top 24 bits of CasparCG's int32 samples
        ac->sample_rate = fmt.sample_rate;
#if FFMPEG_NEW_CHANNEL_LAYOUT
        av_channel_layout_default(&ac->ch_layout, fmt.audio_channels);
#else
        ac->channels       = fmt.audio_channels;
        ac->channel_layout = av_get_default_channel_layout(fmt.audio_channels);
#endif
        ac->time_base = av_make_q(1, fmt.sample_rate);
        if (oc->oformat->flags & AVFMT_GLOBALHEADER)
            ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((e = avcodec_open2(ac, acodec, nullptr)) < 0)
            return "pcm_s24le would not open: " + av_err(e);
        as = avformat_new_stream(oc, nullptr);
        avcodec_parameters_from_context(as->codecpar, ac);
        as->time_base = ac->time_base;
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE) && (e = avio_open(&oc->pb, out.string().c_str(), AVIO_FLAG_WRITE)) < 0)
        return "cannot write " + out.string() + ": " + av_err(e);
    if ((e = avformat_write_header(oc, nullptr)) < 0)
        return "cannot write the header: " + av_err(e);

    AVPacket* pkt = av_packet_alloc();
    std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt_guard(pkt, [](AVPacket* p) { av_packet_free(&p); });
    const auto drain = [&](AVCodecContext* c, AVStream* s) -> std::string {
        while (true) {
            const int r = avcodec_receive_packet(c, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                return {};
            if (r < 0)
                return "encode failed: " + av_err(r);
            av_packet_rescale_ts(pkt, c->time_base, s->time_base);
            pkt->stream_index = s->index;
            const int w       = av_interleaved_write_frame(oc, pkt);
            if (w < 0)
                return "write failed: " + av_err(w);
        }
    };

    AVFrame* vf = av_frame_alloc();
    std::unique_ptr<AVFrame, void (*)(AVFrame*)> vf_guard(vf, [](AVFrame* f) { av_frame_free(&f); });
    vf->format = AV_PIX_FMT_YUV422P10LE;
    vf->width  = fmt.width;
    vf->height = fmt.height;
    if ((e = av_frame_get_buffer(vf, 64)) < 0)
        return "cannot allocate a frame: " + av_err(e);

    cpu_decoder dec(fmt);
    int64_t     video_pts = 0, audio_pts = 0, frames = 0;
    std::vector<uint8_t> audio, video;

    for (const auto& j : jobs) {
        auto src = open_source(j.input);
        if (!src)
            return "cannot open recording " + u8(j.input);
        const auto& f = src->fmt();
        if (f.width != fmt.width || f.height != fmt.height || f.video_codec != fmt.video_codec ||
            f.rate_num * fmt.rate_den != fmt.rate_num * f.rate_den)
            return u8(j.input) + " does not match the first recording's format";
        const int64_t end = j.out < 0 ? src->count() : std::min(j.out, src->count());
        for (int64_t p = std::max<int64_t>(0, j.in); p < end; ++p) {
            store::record_meta m;
            if (!src->read(p, m, audio, video))
                continue; // a record that does not validate is skipped, as playback skips it
            if ((e = av_frame_make_writable(vf)) < 0)
                return "frame not writable: " + av_err(e);
            if (!dec.decode(video, vf))
                continue;
            vf->pts = video_pts++;
            if ((e = avcodec_send_frame(vc, vf)) < 0)
                return "encode failed: " + av_err(e);
            if (auto err = drain(vc, vs); !err.empty())
                return err;

            if (ac && !audio.empty()) {
                const int nb = static_cast<int>(audio.size() / sizeof(int32_t) / fmt.audio_channels);
                AVFrame*  af = av_frame_alloc();
                std::unique_ptr<AVFrame, void (*)(AVFrame*)> af_guard(af, [](AVFrame* fr) { av_frame_free(&fr); });
                af->format      = AV_SAMPLE_FMT_S32;
                af->sample_rate = fmt.sample_rate;
#if FFMPEG_NEW_CHANNEL_LAYOUT
                av_channel_layout_copy(&af->ch_layout, &ac->ch_layout);
#else
                af->channels       = ac->channels;
                af->channel_layout = ac->channel_layout;
#endif
                af->nb_samples = nb;
                if (nb > 0 && av_frame_get_buffer(af, 0) >= 0) {
                    std::memcpy(af->data[0], audio.data(), static_cast<size_t>(nb) * fmt.audio_channels * sizeof(int32_t));
                    af->pts = audio_pts;
                    audio_pts += nb;
                    if ((e = avcodec_send_frame(ac, af)) < 0)
                        return "audio encode failed: " + av_err(e);
                    if (auto err = drain(ac, as); !err.empty())
                        return err;
                }
            }
            if (++frames % 25 == 0)
                publish("running", frames, {});
        }
    }

    avcodec_send_frame(vc, nullptr);
    if (auto err = drain(vc, vs); !err.empty())
        return err;
    if (ac) {
        avcodec_send_frame(ac, nullptr);
        if (auto err = drain(ac, as); !err.empty())
            return err;
    }
    if ((e = av_write_trailer(oc)) < 0)
        return "cannot finish the file: " + av_err(e);
    publish("running", frames, {});
    return frames > 0 ? std::string{} : std::string("no frame was exported");
}

} // namespace

std::future<std::wstring> replay_producer::start_export(const std::vector<std::wstring>& params)
{
    // EXPORT <out> ( [<input>] [in] [out] )...  -- the v1 grammar: a token that reads as a
    // position or a time is a point, anything else names a recording.
    if (params.size() < 2)
        CASPAR_THROW_EXCEPTION(caspar::user_error() << msg_info("[replay] EXPORT needs an output file"));

    std::vector<job> jobs;
    {
        size_t i = 2;
        const auto is_point = [&](const std::wstring& s) {
            int64_t v = 0;
            try {
                size_t used = 0;
                v           = std::stoll(s, &used);
                if (used == s.size())
                    return v >= 0;
            } catch (const std::exception&) {
            }
            return s.find(L':') != std::wstring::npos || std::count(s.begin(), s.end(), L'-') >= 6;
        };
        if (i >= params.size())
            jobs.push_back({display_name_, 0, -1});
        while (i < params.size()) {
            job j{display_name_, 0, -1};
            if (!is_point(params[i]))
                j.input = params[i++];
            // Points resolve against THIS producer's recording when the job is this recording,
            // otherwise against the named one, which export opens on its own.
            auto resolve = [&](const std::wstring& s) -> int64_t {
                if (j.input == display_name_)
                    return parse_point(s);
                auto other = open_source(j.input);
                if (!other)
                    CASPAR_THROW_EXCEPTION(caspar::user_error()
                                           << msg_info(u8(L"[replay] EXPORT cannot open " + j.input)));
                int64_t v = 0;
                try {
                    size_t used = 0;
                    v           = std::stoll(s, &used);
                    if (used == s.size())
                        return v;
                } catch (const std::exception&) {
                }
                CASPAR_THROW_EXCEPTION(caspar::user_error()
                                       << msg_info(u8(L"[replay] EXPORT takes positions for another recording, not '" + s + L"'")));
            };
            if (i < params.size() && is_point(params[i]))
                j.in = resolve(params[i++]);
            if (i < params.size() && is_point(params[i]))
                j.out = resolve(params[i++]);
            jobs.push_back(j);
        }
    }

    // BUSY is a refusal (403 CALL FAILED, the reason in the log), not text inside a 201: a
    // client reading "201 CALL OK" would take a busy export for a started one.
    if (export_running_.exchange(true))
        CASPAR_THROW_EXCEPTION(caspar::user_error()
                               << msg_info("[replay] EXPORT BUSY: one export at a time, and one is running"));
    if (export_thread_.joinable())
        export_thread_.join();

    boost::filesystem::path out(params[1]);
    if (!out.is_absolute())
        out = boost::filesystem::path(env::media_folder()) / out;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_["export/state"]  = std::string("running");
        state_["export/path"]   = out.wstring();
        state_["export/frames"] = int64_t{0};
        state_["export/error"]  = std::string();
    }

    try {
        export_thread_ = std::thread([this, jobs, out] {
            const auto publish = [this](const char* st, int64_t frames, const std::string& err) {
                export_frames_.store(frames);
                std::lock_guard<std::mutex> lock(state_mutex_);
                state_["export/state"]  = std::string(st);
                state_["export/frames"] = frames;
                if (!err.empty())
                    state_["export/error"] = err;
            };
            std::string err;
            try {
                err = run_export(jobs, out, publish);
            } catch (const std::exception& e) {
                err = e.what();
            }
            const int64_t frames = export_frames_.load();
            if (err.empty()) {
                publish("done", frames, {});
                CASPAR_LOG(info) << print() << L" EXPORT done: " << out.wstring() << L" (" << frames << L" frames)";
            } else {
                publish("failed", frames, err);
                CASPAR_LOG(error) << print() << L" EXPORT FAILED: " << out.wstring() << L": " << u16(err);
            }
            export_running_.store(false);
        });
    } catch (...) {
        export_running_.store(false);
        throw;
    }
    // No CRLF of its own: CALL adds one after the data line, and a second one is an extra empty
    // line on the wire that desynchronises a client reading one reply per command.
    return make_ready_future(std::wstring(L"EXPORT STARTED"));
}

}} // namespace caspar::replay
