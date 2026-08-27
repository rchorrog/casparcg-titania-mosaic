#include "av_producer.h"

#include "av_input.h"

#include "../util/av_assert.h"
#include "../util/av_util.h"

#include <boost/exception/exception.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/range/algorithm/rotate.hpp>
#include <boost/rational.hpp>
#include <boost/thread.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>

#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/except.h>
#include <common/executor.h>
#include <common/os/thread.h>
#include <common/scope_exit.h>
#include <common/timer.h>

#include <core/frame/draw_frame.h>
#include <core/frame/frame_factory.h>
#include <core/monitor/monitor.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <iomanip>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

namespace caspar { namespace ffmpeg {

const AVRational TIME_BASE_Q = {1, AV_TIME_BASE};

struct Frame
{
    std::shared_ptr<AVFrame> video;
    std::shared_ptr<AVFrame> audio;
    core::draw_frame         frame;
    int64_t                  start_time  = AV_NOPTS_VALUE;
    int64_t                  pts         = AV_NOPTS_VALUE;
    int64_t                  duration    = 0;
    int64_t                  frame_count = 0;
};

AVPixelFormat get_pix_fmt_with_alpha(AVPixelFormat fmt)
{
    switch (fmt) {
        case AV_PIX_FMT_YUV420P:
            return AV_PIX_FMT_YUVA420P;
        case AV_PIX_FMT_YUV422P:
            return AV_PIX_FMT_YUVA422P;
        case AV_PIX_FMT_YUV444P:
            return AV_PIX_FMT_YUVA444P;
        default:
            break;
    }
    return fmt;
}

const AVCodec* get_decoder(AVCodecID codec_id)
{
    // enforce use of libvpx for vp8 and vp9 codecs to be able
    // to decode webm files with alpha channel
    const AVCodec* result = nullptr;
    if (codec_id == AV_CODEC_ID_VP9)
        result = avcodec_find_decoder_by_name("libvpx-vp9");
    else if (codec_id == AV_CODEC_ID_VP8)
        result = avcodec_find_decoder_by_name("libvpx");
    return result != nullptr ? result : avcodec_find_decoder(codec_id);
}

// TODO (fix) Handle ts discontinuities.
// TODO (feat) Forward options.

core::color_space get_color_space(const std::shared_ptr<AVFrame>& video)
{
    auto result = core::color_space::bt709;
    if (video) {
        switch (video->colorspace) {
            case AVColorSpace::AVCOL_SPC_BT2020_NCL:
                result = core::color_space::bt2020;
                break;
            case AVColorSpace::AVCOL_SPC_BT470BG:
            case AVColorSpace::AVCOL_SPC_SMPTE170M:
            case AVColorSpace::AVCOL_SPC_SMPTE240M:
                result = core::color_space::bt601;
                break;
            default:
                break;
        }
    }

    return result;
}

class Decoder
{
    Decoder(const Decoder&)            = delete;
    Decoder& operator=(const Decoder&) = delete;

    AVStream*         st       = nullptr;
    int64_t           next_pts = AV_NOPTS_VALUE;
    std::atomic<bool> eof      = {false};

    /// Set by the decode thread when the source's timeline jumps; consumed once by the
    /// producer thread, which rebuilds the filter graph. See the detection site below for why
    /// it cannot be done anywhere later.
    std::atomic<bool> discontinuity = {false};

    /// Latest decoded SOURCE timestamp, in AV_TIME_BASE units; AV_NOPTS_VALUE until the first
    /// frame carries one. Written by the decode thread, read by the producer thread for state.
    std::atomic<int64_t> source_time = {AV_NOPTS_VALUE};

    std::queue<std::shared_ptr<AVPacket>> input;
    mutable boost::mutex                  input_mutex;
    boost::condition_variable             input_cond;
    int                                   input_capacity = 2;

    std::queue<std::shared_ptr<AVFrame>> output;
    mutable boost::mutex                 output_mutex;
    boost::condition_variable            output_cond;
    int                                  output_capacity = 8;

    boost::thread thread;

  public:
    std::shared_ptr<AVCodecContext> ctx;

    Decoder() = default;

    explicit Decoder(AVStream* stream)
        : st(stream)
    {
        const auto codec = get_decoder(stream->codecpar->codec_id);

        if (!codec) {
            FF_RET(AVERROR_DECODER_NOT_FOUND, "avcodec_find_decoder");
        }

        ctx = std::shared_ptr<AVCodecContext>(avcodec_alloc_context3(codec),
                                              [](AVCodecContext* ptr) { avcodec_free_context(&ptr); });

        if (!ctx) {
            FF_RET(AVERROR(ENOMEM), "avcodec_alloc_context3");
        }

        FF(avcodec_parameters_to_context(ctx.get(), stream->codecpar));

        if (stream->metadata != NULL) {
            auto entry = av_dict_get(stream->metadata, "alpha_mode", NULL, AV_DICT_MATCH_CASE);
            if (entry != NULL && entry->value != NULL && *entry->value == '1')
                ctx->pix_fmt = get_pix_fmt_with_alpha(ctx->pix_fmt);
        }

        int thread_count = env::properties().get(L"configuration.ffmpeg.producer.threads", 0);
        FF(av_opt_set_int(ctx.get(), "threads", thread_count, 0));

        ctx->pkt_timebase = stream->time_base;

        if (ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
            ctx->framerate           = av_guess_frame_rate(nullptr, stream, nullptr);
            ctx->sample_aspect_ratio = av_guess_sample_aspect_ratio(nullptr, stream, nullptr);
        } else if (ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
#if !(FFMPEG_NEW_CHANNEL_LAYOUT)
            if (!ctx->channel_layout && ctx->channels) {
                ctx->channel_layout = av_get_default_channel_layout(ctx->channels);
            }
            if (!ctx->channels && ctx->channel_layout) {
                ctx->channels = av_get_channel_layout_nb_channels(ctx->channel_layout);
            }
#endif
        }

        FF(avcodec_open2(ctx.get(), codec, nullptr));

        thread = boost::thread([=]() {
            try {
                while (!thread.interruption_requested()) {
                    auto av_frame = alloc_frame();
                    auto ret      = avcodec_receive_frame(ctx.get(), av_frame.get());

                    if (ret == AVERROR(EAGAIN)) {
                        std::shared_ptr<AVPacket> packet;
                        {
                            boost::unique_lock<boost::mutex> lock(input_mutex);
                            input_cond.wait(lock, [&]() { return !input.empty(); });
                            packet = std::move(input.front());
                            input.pop();
                        }
                        FF(avcodec_send_packet(ctx.get(), packet.get()));
                    } else if (ret == AVERROR_EOF) {
                        avcodec_flush_buffers(ctx.get());
                        av_frame->pts = next_pts;
                        next_pts      = AV_NOPTS_VALUE;
                        eof           = true;

                        {
                            boost::unique_lock<boost::mutex> lock(output_mutex);
                            output_cond.wait(lock, [&]() { return output.size() < output_capacity; });
                            output.push(std::move(av_frame));
                        }
                    } else {
                        FF_RET(ret, "avcodec_receive_frame");

                        // TODO: Maybe Fixed in:
                        // https://github.com/FFmpeg/FFmpeg/commit/33203a08e0a26598cb103508327a1dc184b27bc6
                        // NOTE This is a workaround for DVCPRO HD.
#if LIBAVCODEC_VERSION_MAJOR < 61
                        if (av_frame->width > 1024 && av_frame->interlaced_frame) {
                            av_frame->top_field_first = 1;
                        }
#else
                        if (av_frame->width > 1024 && (av_frame->flags & AV_FRAME_FLAG_INTERLACED)) {
                            av_frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST;
                        }
#endif

                        // TODO (fix) is this always best?
                        av_frame->pts = av_frame->best_effort_timestamp;

                        // ── Discontinuity detection, and it has to be HERE ──────────────
                        // This is the last point where the timestamp is still the source's
                        // own. Downstream, `vf_fps` replaces it with a counter
                        // (`libavfilter/vf_fps.c:305`: `frame->pts = s->next_pts++`), so a
                        // jump is undetectable from `Frame::pts` -- and worse, `vf_fps` never
                        // re-seeds that counter, so after a jump it silently drops every
                        // frame (backward) or duplicates one (forward) until the gap is made
                        // up. A 33-bit MPEG-TS PTS wrap is ~26.5 h, so on a 24/7 wall that is
                        // a tile frozen for a day, with a healthy-looking buffer and no
                        // underflow tag anywhere.
                        //
                        // Threshold, not equality: normal jitter and B-frame reordering move
                        // the timestamp around by a frame or two. Five seconds in stream time
                        // is far past anything legitimate and far below a wrap.
                        if (next_pts != AV_NOPTS_VALUE && av_frame->pts != AV_NOPTS_VALUE) {
                            const auto threshold = av_rescale_q(5 * AV_TIME_BASE, {1, AV_TIME_BASE}, st->time_base);
                            if (std::abs(av_frame->pts - next_pts) > threshold) {
                                discontinuity = true;
                            }
                        }

                        // Publish the SOURCE timestamp, for the same reason the check above sits
                        // here: this is the last place it exists. Everything downstream -- and
                        // that includes `file/time`, which is what an operator reads over OSC --
                        // is derived from the filter graph's output, i.e. from `vf_fps`'s counter
                        // rather than from the stream. The two agree while nothing is dropped and
                        // diverge silently afterwards, which is exactly the failure worth
                        // catching, and until now there was no way to see it happen.
                        //
                        // Rescaled to AV_TIME_BASE so it can be compared across streams whose
                        // time bases differ; comparing raw ticks between two inputs is wrong.
                        if (av_frame->pts != AV_NOPTS_VALUE) {
                            source_time = av_rescale_q(av_frame->pts, st->time_base, TIME_BASE_Q);
                        }

#if LIBAVUTIL_VERSION_MAJOR < 58
                        auto duration_pts = av_frame->pkt_duration;
#else
                        auto duration_pts = av_frame->duration;
#endif
                        if (duration_pts <= 0) {
                            if (ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
#if LIBAVCODEC_VERSION_MAJOR < 62
                                const int ticks_per_frame = ctx->ticks_per_frame;
#else
                                // https://github.com/FFmpeg/FFmpeg/commit/e930b834a928546f9cbc937f6633709053448232#diff-115616f8a2b59cab3aac4e7f4c8c31e69e94e7fcfa339b9f65b0bf34308aa80fR682
                                const int ticks_per_frame =
                                    (ctx->codec_descriptor && (ctx->codec_descriptor->props & AV_CODEC_PROP_FIELDS))
                                        ? 2
                                        : 1;
#endif
                                const auto ticks = av_stream_get_parser(st) ? av_stream_get_parser(st)->repeat_pict + 1
                                                                            : ticks_per_frame;
                                duration_pts     = static_cast<int64_t>(AV_TIME_BASE) * ctx->framerate.den * ticks /
                                               ctx->framerate.num / ticks_per_frame;
                                duration_pts = av_rescale_q(duration_pts, {1, AV_TIME_BASE}, st->time_base);
                            } else if (ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
                                duration_pts = av_rescale_q(av_frame->nb_samples, {1, ctx->sample_rate}, st->time_base);
                            }
                        }

                        if (duration_pts > 0) {
                            next_pts = av_frame->pts + duration_pts;
                        } else {
                            next_pts = AV_NOPTS_VALUE;
                        }

                        {
                            boost::unique_lock<boost::mutex> lock(output_mutex);
                            output_cond.wait(lock, [&]() { return output.size() < output_capacity; });
                            output.push(std::move(av_frame));
                        }
                    }
                }
            } catch (boost::thread_interrupted&) {
                // Do nothing...
            } catch (...) {
                eof = true;
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        });
    }

    ~Decoder()
    {
        try {
            if (thread.joinable()) {
                thread.interrupt();
                thread.join();
            }
        } catch (boost::thread_interrupted&) {
            // Do nothing...
        }
    }

    /// True once since the last call: this stream's timeline jumped. Consumed by the producer
    /// thread, which rebuilds the filter graph so `vf_fps` re-seeds its output counter.
    bool take_discontinuity() { return discontinuity.exchange(false); }

    /// Source-side clock, AV_TIME_BASE units. See the write site for why this is not the same
    /// thing as `Frame::pts`.
    int64_t take_source_time() const { return source_time.load(); }

    bool want_packet() const
    {
        if (eof) {
            return false;
        }

        {
            boost::lock_guard<boost::mutex> lock(input_mutex);
            return input.size() < input_capacity;
        }
    }

    void push(std::shared_ptr<AVPacket> packet)
    {
        if (eof) {
            return;
        }

        {
            boost::lock_guard<boost::mutex> lock(input_mutex);
            input.push(std::move(packet));
        }

        input_cond.notify_all();
    }

    std::shared_ptr<AVFrame> pop()
    {
        std::shared_ptr<AVFrame> frame;

        {
            boost::lock_guard<boost::mutex> lock(output_mutex);

            if (!output.empty()) {
                frame = std::move(output.front());
                output.pop();
            }
        }

        if (frame) {
            output_cond.notify_all();
        } else if (eof) {
            frame = alloc_frame();
        }

        return frame;
    }
};

struct Filter
{
    std::shared_ptr<AVFilterGraph>  graph;
    AVFilterContext*                sink = nullptr;
    std::map<int, AVFilterContext*> sources;
    std::shared_ptr<AVFrame>        frame;
    bool                            eof = false;

    Filter() = default;

    Filter(std::string                    filter_spec,
           const Input&                   input,
           std::map<int, Decoder>&        streams,
           int64_t                        start_time,
           AVMediaType                    media_type,
           const core::video_format_desc& format_desc)
    {
        if (media_type == AVMEDIA_TYPE_VIDEO) {
            if (filter_spec.empty()) {
                filter_spec = "null";
            }

            auto deint = u8(
                env::properties().get<std::wstring>(L"configuration.ffmpeg.producer.auto-deinterlace", L"interlaced"));

            if (deint != "none") {
                filter_spec += (boost::format(",bwdif=mode=send_field:parity=auto:deint=%s") % deint).str();
            }

            filter_spec += (boost::format(",fps=fps=%d/%d:start_time=%f") %
                            (format_desc.framerate.numerator() * format_desc.field_count) %
                            format_desc.framerate.denominator() % (static_cast<double>(start_time) / AV_TIME_BASE))
                               .str();
        } else if (media_type == AVMEDIA_TYPE_AUDIO) {
            if (filter_spec.empty()) {
                filter_spec = "anull";
            }

            // Find first audio stream to get a time_base for the first_pts calculation
            AVRational tb = {1, format_desc.audio_sample_rate};
            for (auto n = 0U; n < input->nb_streams; ++n) {
                const auto st = input->streams[n];
#if FFMPEG_NEW_CHANNEL_LAYOUT
                const auto codec_channels = st->codecpar->ch_layout.nb_channels;
#else
                const auto codec_channels = st->codecpar->channels;
#endif
                if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && codec_channels > 0) {
                    tb = {1, st->codecpar->sample_rate};
                    break;
                }
            }
            filter_spec += (boost::format(",aresample=async=1000:first_pts=%d:min_comp=0.01:osr=%d,"
                                          "asetnsamples=n=1024:p=0") %
                            av_rescale_q(start_time, TIME_BASE_Q, tb) % format_desc.audio_sample_rate)
                               .str();
        }

        AVFilterInOut* outputs = nullptr;
        AVFilterInOut* inputs  = nullptr;

        CASPAR_SCOPE_EXIT
        {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
        };

        int video_input_count = 0;
        int audio_input_count = 0;
        {
            auto graph2 = avfilter_graph_alloc();
            if (!graph2) {
                FF_RET(AVERROR(ENOMEM), "avfilter_graph_alloc");
            }

            CASPAR_SCOPE_EXIT
            {
                avfilter_graph_free(&graph2);
                avfilter_inout_free(&inputs);
                avfilter_inout_free(&outputs);
            };

            FF(avfilter_graph_parse2(graph2, filter_spec.c_str(), &inputs, &outputs));

            for (auto cur = inputs; cur; cur = cur->next) {
                const auto type = avfilter_pad_get_type(cur->filter_ctx->input_pads, cur->pad_idx);
                if (type == AVMEDIA_TYPE_VIDEO) {
                    video_input_count += 1;
                } else if (type == AVMEDIA_TYPE_AUDIO) {
                    audio_input_count += 1;
                }
            }
        }

        std::vector<AVStream*> av_streams;
        for (auto n = 0U; n < input->nb_streams; ++n) {
            const auto st = input->streams[n];

#if FFMPEG_NEW_CHANNEL_LAYOUT
            const auto codec_channels = st->codecpar->ch_layout.nb_channels;
#else
            const auto codec_channels = st->codecpar->channels;
#endif
            if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && codec_channels == 0) {
                continue;
            }

            auto disposition = st->disposition;
            if (!disposition || disposition == AV_DISPOSITION_DEFAULT) {
                av_streams.push_back(st);
            }
        }

        if (audio_input_count == 1) {
            auto count = std::count_if(av_streams.begin(), av_streams.end(), [](auto s) {
                return s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
            });

            // TODO (fix) Use some form of stream meta data to do this.
            // https://github.com/CasparCG/server/issues/833
            if (count > 1) {
                filter_spec = (boost::format("amerge=inputs=%d,") % count).str() + filter_spec;
            }
        }

        if (video_input_count == 1) {
            std::stable_sort(av_streams.begin(), av_streams.end(), [](auto lhs, auto rhs) {
                return lhs->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && lhs->codecpar->height > rhs->codecpar->height;
            });

            std::vector<AVStream*> video_av_streams;
            std::copy_if(av_streams.begin(), av_streams.end(), std::back_inserter(video_av_streams), [](auto s) {
                return s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
            });

            // TODO (fix) Use some form of stream meta data to do this.
            // https://github.com/CasparCG/server/issues/832
            if (video_av_streams.size() >= 2 &&
                video_av_streams[0]->codecpar->height == video_av_streams[1]->codecpar->height) {
                filter_spec = "alphamerge," + filter_spec;
            }
        }

        graph = std::shared_ptr<AVFilterGraph>(avfilter_graph_alloc(),
                                               [](AVFilterGraph* ptr) { avfilter_graph_free(&ptr); });

        if (!graph) {
            FF_RET(AVERROR(ENOMEM), "avfilter_graph_alloc");
        }

        FF(avfilter_graph_parse2(graph.get(), filter_spec.c_str(), &inputs, &outputs));

        // inputs
        {
            for (auto cur = inputs; cur; cur = cur->next) {
                const auto type = avfilter_pad_get_type(cur->filter_ctx->input_pads, cur->pad_idx);
                if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) {
                    CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                            << msg_info_t("only video and audio filters supported"));
                }

                unsigned index = 0;

                // TODO find stream based on link name
                while (true) {
                    if (index == av_streams.size()) {
                        graph = nullptr;
                        return;
                    }
                    if (av_streams.at(index)->codecpar->codec_type == type &&
                        sources.find(static_cast<int>(index)) == sources.end()) {
                        break;
                    }
                    index++;
                }

                index = av_streams.at(index)->index;

                auto it = streams.find(index);
                if (it == streams.end()) {
                    it = streams.emplace(index, input->streams[index]).first;
                }

                auto st = it->second.ctx;

                if (st->codec_type == AVMEDIA_TYPE_VIDEO) {
                    auto args = (boost::format("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d") % st->width % st->height %
                                 st->pix_fmt % st->pkt_timebase.num % st->pkt_timebase.den)
                                    .str();
                    auto name = (boost::format("in_%d") % index).str();

                    if (st->sample_aspect_ratio.num > 0 && st->sample_aspect_ratio.den > 0) {
                        args +=
                            (boost::format(":sar=%d/%d") % st->sample_aspect_ratio.num % st->sample_aspect_ratio.den)
                                .str();
                    }

                    if (st->framerate.num > 0 && st->framerate.den > 0) {
                        args += (boost::format(":frame_rate=%d/%d") % st->framerate.num % st->framerate.den).str();
                    }

                    AVFilterContext* source = nullptr;
                    FF(avfilter_graph_create_filter(
                        &source, avfilter_get_by_name("buffer"), name.c_str(), args.c_str(), nullptr, graph.get()));
                    FF(avfilter_link(source, 0, cur->filter_ctx, cur->pad_idx));
                    sources.emplace(index, source);
                } else if (st->codec_type == AVMEDIA_TYPE_AUDIO) {
#if FFMPEG_NEW_CHANNEL_LAYOUT
                    char channel_layout[128];
                    FF(av_channel_layout_describe(&st->ch_layout, channel_layout, sizeof(channel_layout)));
#else
                    const auto channel_layout = st->channel_layout;
#endif

                    auto args = (boost::format("time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%#x") %
                                 st->pkt_timebase.num % st->pkt_timebase.den % st->sample_rate %
                                 av_get_sample_fmt_name(st->sample_fmt) % channel_layout)
                                    .str();
                    auto name = (boost::format("in_%d") % index).str();

                    AVFilterContext* source = nullptr;
                    FF(avfilter_graph_create_filter(
                        &source, avfilter_get_by_name("abuffer"), name.c_str(), args.c_str(), nullptr, graph.get()));
                    FF(avfilter_link(source, 0, cur->filter_ctx, cur->pad_idx));
                    sources.emplace(index, source);
                } else {
                    CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                            << msg_info_t("invalid filter input media type"));
                }
            }
        }

        if (media_type == AVMEDIA_TYPE_VIDEO) {
            FF(avfilter_graph_create_filter(
                &sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph.get()));

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4245)
#endif
            const AVPixelFormat pix_fmts[] = {AV_PIX_FMT_RGB24,
                                              AV_PIX_FMT_BGR24,
                                              AV_PIX_FMT_BGRA,
                                              AV_PIX_FMT_ARGB,
                                              AV_PIX_FMT_RGBA,
                                              AV_PIX_FMT_ABGR,
                                              AV_PIX_FMT_YUV444P,
                                              AV_PIX_FMT_YUV444P10,
                                              AV_PIX_FMT_YUV444P12,
                                              AV_PIX_FMT_YUV422P,
                                              AV_PIX_FMT_YUV422P10,
                                              AV_PIX_FMT_YUV422P12,
                                              AV_PIX_FMT_YUV420P,
                                              AV_PIX_FMT_YUV420P10,
                                              AV_PIX_FMT_YUV420P12,
                                              AV_PIX_FMT_YUV410P,
                                              AV_PIX_FMT_YUVA444P,
                                              AV_PIX_FMT_YUVA422P,
                                              AV_PIX_FMT_YUVA420P,
                                              AV_PIX_FMT_UYVY422,
                                              // bwdif needs planar rgb
                                              AV_PIX_FMT_GBRP,
                                              AV_PIX_FMT_GBRP10,
                                              AV_PIX_FMT_GBRP12,
                                              AV_PIX_FMT_GBRP16,
                                              AV_PIX_FMT_GBRAP,
                                              AV_PIX_FMT_GBRAP16,
                                              AV_PIX_FMT_NONE};
            FF(av_opt_set_int_list(sink, "pix_fmts", pix_fmts, -1, AV_OPT_SEARCH_CHILDREN));
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        } else if (media_type == AVMEDIA_TYPE_AUDIO) {
            FF(avfilter_graph_create_filter(
                &sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph.get()));
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4245)
#endif
            const AVSampleFormat sample_fmts[] = {AV_SAMPLE_FMT_S32, AV_SAMPLE_FMT_NONE};
            FF(av_opt_set_int_list(sink, "sample_fmts", sample_fmts, -1, AV_OPT_SEARCH_CHILDREN));

            FF(av_opt_set_int(sink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN));

            const int sample_rates[] = {format_desc.audio_sample_rate, -1};
            FF(av_opt_set_int_list(sink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN));
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        } else {
            CASPAR_THROW_EXCEPTION(ffmpeg_error_t()
                                   << boost::errinfo_errno(EINVAL) << msg_info_t("invalid output media type"));
        }

        // output
        {
            const auto cur = outputs;

            if (!cur || cur->next) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter graph output count"));
            }

            if (avfilter_pad_get_type(cur->filter_ctx->output_pads, cur->pad_idx) != media_type) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter output media type"));
            }

            FF(avfilter_link(cur->filter_ctx, cur->pad_idx, sink, 0));
        }

        FF(avfilter_graph_config(graph.get(), nullptr));

        CASPAR_LOG(debug) << avfilter_graph_dump(graph.get(), nullptr);
    }

    bool operator()(int nb_samples = -1)
    {
        if (frame || eof) {
            return false;
        }

        if (!sink || sources.empty()) {
            eof   = true;
            frame = nullptr;
            return true;
        }

        auto av_frame = alloc_frame();
        auto ret      = nb_samples >= 0 ? av_buffersink_get_samples(sink, av_frame.get(), nb_samples)
                                        : av_buffersink_get_frame(sink, av_frame.get());

        if (ret == AVERROR(EAGAIN)) {
            return false;
        }
        if (ret == AVERROR_EOF) {
            eof   = true;
            frame = nullptr;
            return true;
        }
        FF_RET(ret, "av_buffersink_get_frame");
        frame = std::move(av_frame);
        return true;
    }
};

struct AVProducer::Impl
{
    caspar::core::monitor::state state_;
    mutable boost::mutex         state_mutex_;

    spl::shared_ptr<diagnostics::graph> graph_;

    const std::shared_ptr<core::frame_factory> frame_factory_;
    const core::video_format_desc              format_desc_;
    const AVRational                           format_tb_;
    const std::string                          name_;
    const std::string                          path_;

    Input                  input_;
    std::map<int, Decoder> decoders_;
    Filter                 video_filter_;
    Filter                 audio_filter_;

    std::map<int, std::vector<AVFilterContext*>> sources_;

    std::atomic<int64_t> start_{AV_NOPTS_VALUE};
    std::atomic<int64_t> duration_{AV_NOPTS_VALUE};
    std::atomic<int64_t> input_duration_{AV_NOPTS_VALUE};
    std::atomic<int64_t> seek_{AV_NOPTS_VALUE};
    std::atomic<bool>    loop_{false};

    std::string afilter_;
    std::string vfilter_;

    int                              seekable_ = 2;
    core::frame_geometry::scale_mode scale_mode_;
    int64_t                          frame_count_    = 0;
    bool                             frame_flush_    = true;
    int64_t                          frame_time_     = AV_NOPTS_VALUE;
    int64_t                          frame_duration_ = AV_NOPTS_VALUE;
    core::draw_frame                 frame_;

    std::deque<Frame>         buffer_;
    mutable boost::mutex      buffer_mutex_;
    boost::condition_variable buffer_cond_;
    std::atomic<bool>         buffer_eof_{false};
    int                       buffer_capacity_ = static_cast<int>(format_desc_.fps) / 4;

    std::optional<caspar::executor> video_executor_;
    std::optional<caspar::executor> audio_executor_;

    int latency_ = 0;

    // ── Rate governor ──────────────────────────────────────────────────────────────────
    // Twenty encoders and the host each run a free crystal, and nothing locks them together.
    // A source fractionally FASTER than the channel fills `buffer_`, parks the producer thread
    // on `buffer_cond_`, back-pressures the demuxer and eventually overflows the socket --
    // packet loss and macroblocking, not a clean drop. A source fractionally SLOWER starves,
    // and each starve costs a frame of latency that is never given back, because underflow
    // repeats without consuming.
    //
    // So the system can currently only repeat or corrupt; it has no way to DROP. The governor
    // adds that one missing direction, and reports the offset it is correcting for, in ppm,
    // which is the number that identifies which encoder is actually at fault.
    //
    // Deliberately NOT timestamp-based: `vf_fps` replaces every PTS with a counter
    // (`libavfilter/vf_fps.c:305`), so downstream timestamps carry no source timing at all and
    // scheduling on them would be `pop_front()` in disguise.
    bool   governor_enabled_ = false;
    int    governor_target_  = 4;
    int    governor_band_    = 2;
    int    governor_min_gap_ = 25;
    double occupancy_avg_    = 0.0;
    bool   occupancy_primed_ = false;

    int64_t ticks_            = 0;
    int64_t drops_            = 0;
    int64_t repeats_          = 0;
    int64_t discontinuities_  = 0;
    int64_t last_correction_  = 0;

    /// Mirror of `buffer_.size()`, published under `buffer_mutex_` and read without it.
    ///
    /// `update_state()` runs from a `CASPAR_SCOPE_EXIT` in several functions, some of which
    /// hold `buffer_mutex_`; `boost::mutex` is not recursive, so reading the deque there could
    /// deadlock depending on declaration order. An atomic sidesteps the question entirely.
    std::atomic<int64_t> buffer_size_{0};

    boost::thread thread_;

    Impl(std::shared_ptr<core::frame_factory> frame_factory,
         core::video_format_desc              format_desc,
         std::string                          name,
         std::string                          path,
         std::string                          vfilter,
         std::string                          afilter,
         std::optional<int64_t>               start,
         std::optional<int64_t>               seek,
         std::optional<int64_t>               duration,
         bool                                 loop,
         int                                  seekable,
         core::frame_geometry::scale_mode     scale_mode)
        : frame_factory_(frame_factory)
        , format_desc_(format_desc)
        , format_tb_({format_desc.duration, format_desc.time_scale * format_desc.field_count})
        , name_(name)
        , path_(path)
        , input_(path, graph_, seekable >= 0 && seekable < 2 ? std::optional<bool>(false) : std::optional<bool>())
        , start_(start ? av_rescale_q(*start, format_tb_, TIME_BASE_Q) : AV_NOPTS_VALUE)
        , duration_(duration ? av_rescale_q(*duration, format_tb_, TIME_BASE_Q) : AV_NOPTS_VALUE)
        , loop_(loop)
        , afilter_(afilter)
        , vfilter_(vfilter)
        , seekable_(seekable)
        , scale_mode_(scale_mode)
        , video_executor_(L"video-executor")
        , audio_executor_(L"audio-executor")
    {
        diagnostics::register_graph(graph_);
        graph_->set_color("underflow", diagnostics::color(0.6f, 0.3f, 0.9f));
        graph_->set_color("frame-time", diagnostics::color(0.0f, 1.0f, 0.0f));
        graph_->set_color("decode-time", diagnostics::color(0.0f, 1.0f, 1.0f));
        graph_->set_color("buffer", diagnostics::color(1.0f, 1.0f, 0.0f));
        graph_->set_color("drop", diagnostics::color(1.0f, 0.3f, 0.0f));
        graph_->set_color("discontinuity", diagnostics::color(1.0f, 0.0f, 1.0f));

        // Same read-at-point-of-use pattern the module already uses for `threads` and
        // `auto-deinterlace`; no plumbing, and absent config means today's behaviour exactly.
        governor_enabled_ = env::properties().get(L"configuration.ffmpeg.producer.sync.enabled", false);
        governor_target_ =
            std::max(1, env::properties().get(L"configuration.ffmpeg.producer.sync.target-frames", 4));
        governor_band_ =
            std::max(1, env::properties().get(L"configuration.ffmpeg.producer.sync.deadband-frames", 2));
        governor_min_gap_ =
            std::max(1, env::properties().get(L"configuration.ffmpeg.producer.sync.min-interval-frames", 25));

        // The governor needs headroom ABOVE its target or it can never see "too full" -- and
        // the producer thread parking on `buffer_cond_` is what causes packet loss on a fast
        // feed, which is the failure the governor exists to prevent.
        if (governor_enabled_) {
            buffer_capacity_ = std::max(buffer_capacity_, governor_target_ * 3 + 2);
        }

        state_["file/name"] = u8(name_);
        state_["file/path"] = u8(path_);
        state_["loop"]      = loop;
        update_state();

        CASPAR_LOG(debug) << print() << " seekable: " << seekable_;

        thread_ = boost::thread([=] {
            try {
                run(seek);
            } catch (boost::thread_interrupted&) {
                // Do nothing...
            } catch (ffmpeg::ffmpeg_error_t& ex) {
                if (auto errn = boost::get_error_info<ffmpeg_errn_info>(ex)) {
                    if (*errn == AVERROR_EXIT) {
                        return;
                    }
                }
                CASPAR_LOG_CURRENT_EXCEPTION();
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        });
    }

    ~Impl()
    {
        input_.abort();

        try {
            if (thread_.joinable()) {
                thread_.interrupt();
                thread_.join();
            }
        } catch (boost::thread_interrupted&) {
            // Do nothing...
        }

        video_executor_.reset();
        audio_executor_.reset();

        CASPAR_LOG(debug) << print() << " Joined";
    }

    void run(std::optional<int64_t> firstSeek)
    {
        std::vector<int> audio_cadence = format_desc_.audio_cadence;

        input_.reset();
        {
            core::monitor::state streams;
            for (auto n = 0UL; n < input_->nb_streams; ++n) {
                auto st                             = input_->streams[n];
                auto framerate                      = av_guess_frame_rate(nullptr, st, nullptr);
                streams[std::to_string(n) + "/fps"] = {framerate.num, framerate.den};
            }

            boost::lock_guard<boost::mutex> lock(state_mutex_);
            state_["file/streams"] = streams;
        }

        if (input_duration_ == AV_NOPTS_VALUE) {
            input_duration_ = input_->duration;
        }

        {
            const auto start = start_.load();
            if (duration_ == AV_NOPTS_VALUE && input_->duration > 0) {
                if (start != AV_NOPTS_VALUE) {
                    duration_ = input_->duration - start;
                } else {
                    duration_ = input_->duration;
                }
            }

            const auto firstStart = firstSeek ? av_rescale_q(*firstSeek, format_tb_, TIME_BASE_Q) : start;
            if (firstStart != AV_NOPTS_VALUE) {
                seek_internal(firstStart);
            } else {
                reset(input_->start_time != AV_NOPTS_VALUE ? input_->start_time : 0);
            }
        }

        set_thread_name(L"[ffmpeg::av_producer]");

        boost::range::rotate(audio_cadence, std::end(audio_cadence) - 1);

        Frame frame;
        timer frame_timer;
        timer decode_timer;

        int warning_debounce = 0;

        while (!thread_.interruption_requested()) {
            {
                const auto seek = seek_.exchange(AV_NOPTS_VALUE);

                if (seek != AV_NOPTS_VALUE) {
                    seek_internal(seek);
                    frame = Frame{};
                    continue;
                }
            }

            {
                // ── Recover from a source-side timeline jump ────────────────────────────
                // A wrap, an encoder restart or a reconnect leaves `vf_fps` comparing new
                // timestamps against a counter seeded from the old ones, and it never
                // re-seeds itself -- so it drops or duplicates every frame until the
                // difference is made up. At a 33-bit wrap that is 26.5 hours of frozen tile.
                //
                // Rebuilding the filters is the existing cure: `reset()` constructs both
                // `Filter`s afresh, which re-runs `vf_fps`'s `config_props` and re-seeds its
                // counter. `seek_internal` already does exactly this for seeks; a
                // discontinuity needs the same treatment without moving the read position,
                // which for a live input is not seekable anyway.
                bool discontinuity = input_.take_discontinuity();
                for (auto& p : decoders_) {
                    // Every decoder is polled, not just the first to report -- `take_` clears
                    // the flag, and leaving one set would fire a second reset next tick.
                    discontinuity = p.second.take_discontinuity() || discontinuity;
                }

                if (discontinuity) {
                    discontinuities_ += 1;
                    if (discontinuities_ == 1 || discontinuities_ % 25 == 0) {
                        // Latched: a flapping encoder must not fill the log across 20 layers.
                        CASPAR_LOG(info) << print() << " timeline discontinuity; resynchronising ("
                                         << discontinuities_ << " so far)";
                    }
                    graph_->set_tag(diagnostics::tag_severity::WARNING, "discontinuity");

                    // Not `seek_internal`: a live stream cannot seek, and asking it to would
                    // fail or reopen. Only the filter graph's notion of time is stale.
                    frame_flush_ = true;
                    reset(input_->start_time != AV_NOPTS_VALUE ? input_->start_time : 0);
                    frame = Frame{};
                    continue;
                }
            }

            {
                // TODO (perf) seek as soon as input is past duration or eof.

                auto start    = start_.load();
                auto duration = duration_.load();

                start       = start != AV_NOPTS_VALUE ? start : 0;
                auto end    = duration != AV_NOPTS_VALUE ? start + duration : INT64_MAX;
                auto time   = frame.pts != AV_NOPTS_VALUE ? frame.pts + frame.duration : 0;
                buffer_eof_ = (video_filter_.eof && audio_filter_.eof) ||
                              av_rescale_q(time, TIME_BASE_Q, format_tb_) >= av_rescale_q(end, TIME_BASE_Q, format_tb_);

                if (buffer_eof_) {
                    if (loop_ && frame_count_ > 2) {
                        frame = Frame{};
                        seek_internal(start);
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    // TODO (fix) Limit live polling due to bugs.
                    continue;
                }
            }

            bool progress = false;
            {
                progress |= schedule();

                std::vector<std::future<bool>> futures;

                if (!video_filter_.frame) {
                    futures.push_back(video_executor_->begin_invoke([&]() { return video_filter_(); }));
                }

                if (!audio_filter_.frame) {
                    futures.push_back(audio_executor_->begin_invoke([&]() { return audio_filter_(audio_cadence[0]); }));
                }

                for (auto& future : futures) {
                    progress |= future.get();
                }
            }

            if ((!video_filter_.frame && !video_filter_.eof) || (!audio_filter_.frame && !audio_filter_.eof)) {
                if (!progress) {
                    if (warning_debounce++ % 500 == 100) {
                        if (!video_filter_.frame && !video_filter_.eof) {
                            CASPAR_LOG(warning) << print() << " Waiting for video frame...";
                        } else if (!audio_filter_.frame && !audio_filter_.eof) {
                            CASPAR_LOG(warning) << print() << " Waiting for audio frame...";
                        } else {
                            CASPAR_LOG(warning) << print() << " Waiting for frame...";
                        }
                    }

                    // TODO (perf): Avoid live loop.
                    std::this_thread::sleep_for(std::chrono::milliseconds(warning_debounce > 25 ? 20 : 5));
                }
                continue;
            }

            warning_debounce = 0;

            // TODO (fix)
            // if (start_ != AV_NOPTS_VALUE && frame.pts < start_) {
            //    seek_internal(start_);
            //    continue;
            //}

            const auto start_time = input_->start_time != AV_NOPTS_VALUE ? input_->start_time : 0;

            if (video_filter_.frame) {
                frame.video      = std::move(video_filter_.frame);
                const auto tb    = av_buffersink_get_time_base(video_filter_.sink);
                const auto fr    = av_buffersink_get_frame_rate(video_filter_.sink);
                frame.start_time = start_time;
                frame.pts        = av_rescale_q(frame.video->pts, tb, TIME_BASE_Q) - start_time;
                frame.duration   = av_rescale_q(1, av_inv_q(fr), TIME_BASE_Q);
            }

            if (audio_filter_.frame) {
                frame.audio      = std::move(audio_filter_.frame);
                const auto tb    = av_buffersink_get_time_base(audio_filter_.sink);
                const auto sr    = av_buffersink_get_sample_rate(audio_filter_.sink);
                frame.start_time = start_time;
                frame.pts        = av_rescale_q(frame.audio->pts, tb, TIME_BASE_Q) - start_time;
                frame.duration   = av_rescale_q(frame.audio->nb_samples, {1, sr}, TIME_BASE_Q);
            }

            frame.frame = core::draw_frame(
                make_frame(this, *frame_factory_, frame.video, frame.audio, get_color_space(frame.video), scale_mode_));
            frame.frame_count = frame_count_++;

            graph_->set_value("decode-time", decode_timer.elapsed() * format_desc_.fps * 0.5);

            {
                boost::unique_lock<boost::mutex> buffer_lock(buffer_mutex_);
                buffer_cond_.wait(buffer_lock, [&] { return buffer_.size() < buffer_capacity_; });
                if (seek_ == AV_NOPTS_VALUE) {
                    buffer_.push_back(frame);
                }
                buffer_size_ = static_cast<int64_t>(buffer_.size());
            }

            if (format_desc_.field_count != 2 || frame_count_ % 2 == 1) {
                // Update the frame-time every other frame when interlaced
                graph_->set_value("frame-time", frame_timer.elapsed() * format_desc_.hz * 0.5);
                frame_timer.restart();
            }

            decode_timer.restart();

            graph_->set_value("buffer", static_cast<double>(buffer_.size()) / static_cast<double>(buffer_capacity_));

            boost::range::rotate(audio_cadence, std::end(audio_cadence) - 1);
        }
    }

    void update_state()
    {
        graph_->set_text(u16(print()));
        boost::lock_guard<boost::mutex> lock(state_mutex_);
        state_["file/clip"] = {start().value_or(0) / format_desc_.fps, duration().value_or(0) / format_desc_.fps};
        state_["file/time"] = {time() / format_desc_.fps, file_duration().value_or(0) / format_desc_.fps};
        state_["loop"]      = loop_;

        // ── Ship the measurement, whether or not the governor is on ─────────────────────
        // These are useful precisely when the correction is DISABLED: they say how far each
        // encoder's clock is from the channel's, so a site can be diagnosed before anything
        // is changed. Turning the governor on without this data first would be guessing.
        state_["sync/mode"]            = governor_enabled_ ? std::string("governed") : std::string("passive");
        // Published because a timestamp source that changes silently is the worst kind: every
        // sync/* number below means something different depending on it. Read here rather than
        // passed from Input, since only live inputs actually apply it -- see av_input.cpp.
        state_["sync/timestamps"] =
            env::properties().get(L"configuration.ffmpeg.producer.sync.wallclock-timestamps", false)
                ? std::string("wallclock")
                : std::string("source");
        state_["sync/repeats"]         = repeats_;
        state_["sync/drops"]           = drops_;
        state_["sync/discontinuities"] = discontinuities_;
        state_["sync/reconnects"]      = input_.reconnects();

        // **The ratchet, named.** Every repeat adds a frame of latency and every drop removes
        // one, so this is the accumulated slip in frames. Flat means governed; climbing means
        // the feed is slipping and nothing is giving the frames back. This single number is
        // what the whole exercise is about.
        state_["sync/net-slip-frames"] = repeats_ - drops_;

        // ── The source clock, and how far the presented one has drifted from it ─────────
        // `file/time` above is derived from the filter graph's output, so it counts frames
        // PRESENTED. `vf_fps` replaces the stream timestamp with its own counter
        // (`vf_fps.c:305`), which means the two agree while nothing is dropped and part company
        // silently when something is -- and there was previously no way to observe that from
        // outside. `sync/source-time` is the stream's own clock, normalised the same way as
        // `file/time` so the two are directly comparable, and `sync/graph-slip-frames` is their
        // difference in frames.
        //
        // A non-zero, growing `graph-slip-frames` means the graph is dropping or duplicating
        // against the source: the failure that presents as a frozen tile with a healthy buffer.
        // For two feeds off ONE encoder -- a main/backup pair -- `source-time` is directly
        // comparable between layers, which `file/time` is not, because each producer normalises
        // against its own `input_->start_time`.
        {
            int64_t src = AV_NOPTS_VALUE;
            for (auto& p : decoders_) {
                if (p.second.ctx && p.second.ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                    src = p.second.take_source_time();
                    break;
                }
            }

            // `operator->` yields the AVFormatContext, which is null between a failed reopen
            // and the next successful one -- a state this build can now actually reach, since
            // the input reconnects instead of dying. The surrounding code runs where it cannot
            // be null; this runs on every state tick, so it can.
            const auto* ic        = input_.operator->();
            const auto  start_time =
                (ic != nullptr && ic->start_time != AV_NOPTS_VALUE) ? ic->start_time : static_cast<int64_t>(0);
            if (src != AV_NOPTS_VALUE) {
                const auto normalised = src - start_time;
                state_["sync/source-time"] = normalised / static_cast<double>(AV_TIME_BASE);
                state_["sync/graph-slip-frames"] =
                    static_cast<int64_t>(std::llround((normalised - frame_time_) /
                                                      static_cast<double>(frame_duration_ > 0 ? frame_duration_ : 1)));
            } else {
                // Said explicitly rather than omitted: a missing field reads as a broken
                // exporter, and "this stream carries no usable timestamps" is a real answer.
                state_["sync/source-time"]       = -1.0;
                state_["sync/graph-slip-frames"] = 0;
            }
        }

        // The offset the governor is correcting for, in parts per million -- i.e. how far this
        // encoder's crystal is from the host's. Identifies WHICH encoder to fix, rather than
        // papering over all twenty.
        state_["sync/offset-ppm"] = ticks_ > 0 ? (static_cast<double>(drops_ - repeats_) / ticks_) * 1e6 : 0.0;

        state_["sync/buffer"]        = buffer_size_.load();
        state_["sync/buffer-avg"]    = occupancy_avg_;
        state_["sync/buffer-target"] = static_cast<int64_t>(governor_target_);
    }

    core::draw_frame prev_frame(const core::video_field field)
    {
        CASPAR_SCOPE_EXIT { update_state(); };

        // Don't start a new frame on the 2nd field
        if (field != core::video_field::b) {
            if (frame_flush_ || !frame_) {
                boost::lock_guard<boost::mutex> lock(buffer_mutex_);

                if (!buffer_.empty()) {
                    frame_          = buffer_[0].frame;
                    frame_time_     = buffer_[0].pts;
                    frame_duration_ = buffer_[0].duration;
                    frame_flush_    = false;
                }
            }
        }

        return core::draw_frame::still(frame_);
    }

    bool is_ready()
    {
        boost::lock_guard<boost::mutex> lock(buffer_mutex_);
        return !buffer_.empty() || frame_;
    }

    /// EWMA weight for a ~4 second time constant.
    ///
    /// Long against transport jitter, short against clock drift -- which is the whole
    /// discrimination the governor rests on. A shorter constant would chase jitter and correct
    /// constantly; a longer one would take minutes to notice a genuinely fast encoder.
    double occupancy_alpha() const { return 1.0 / std::max(1.0, format_desc_.fps * 4.0); }

    core::draw_frame next_frame(const core::video_field field)
    {
        CASPAR_SCOPE_EXIT { update_state(); };

        boost::lock_guard<boost::mutex> lock(buffer_mutex_);

        ticks_ += 1;

        // The post-flush prefill. The stock `4` is unrelated to `buffer_capacity_`, so a layer
        // resumes with 80 ms of runway inside a 240 ms ring and grazes empty on ordinary
        // jitter. Under the governor it follows the configured target instead.
        const auto prefill = governor_enabled_ ? governor_target_ : 4;

        if (buffer_.empty() || (frame_flush_ && static_cast<int>(buffer_.size()) < prefill)) {
            auto start    = start_.load();
            auto duration = duration_.load();

            start    = start != AV_NOPTS_VALUE ? start : 0;
            auto end = duration != AV_NOPTS_VALUE ? start + duration : INT64_MAX;

            if (buffer_eof_ && !frame_flush_) {
                if (frame_time_ < end && frame_duration_ != AV_NOPTS_VALUE) {
                    frame_time_ += frame_duration_;
                } else if (frame_time_ < end) {
                    frame_time_ = input_duration_;
                }
                return core::draw_frame::still(frame_);
            }
            graph_->set_tag(diagnostics::tag_severity::WARNING, "underflow");
            latency_ += 1;
            repeats_ += 1;
            // Occupancy is genuinely zero; let the average see that so the governor does not
            // decide to drop moments after a starve.
            occupancy_avg_ = occupancy_primed_ ? occupancy_avg_ * (1.0 - occupancy_alpha()) : 0.0;
            return core::draw_frame{};
        }

        // ── The governor: the one direction the pipeline is missing ─────────────────────
        // Deliberately placed AFTER the underflow branch and BEFORE the parity guard: it must
        // not run when there is nothing to serve, and a drop must not leave the wrong field at
        // the head of the buffer.
        if (governor_enabled_) {
            const auto occupancy = static_cast<double>(buffer_.size());
            const auto alpha     = occupancy_alpha();
            occupancy_avg_       = occupancy_primed_ ? occupancy_avg_ + alpha * (occupancy - occupancy_avg_) : occupancy;
            occupancy_primed_    = true;

            // Frames are dropped a whole field-pair at a time. Dropping an odd number when
            // interlaced inverts parity, and the guard below would then reject the next call
            // as an underflow -- a correction that manufactures the fault it is fixing.
            const auto step = std::max(1, format_desc_.field_count);

            const bool over      = occupancy_avg_ > (governor_target_ + governor_band_);
            const bool have_room = static_cast<int>(buffer_.size()) > governor_target_ + step;
            const bool settled   = (ticks_ - last_correction_) >= governor_min_gap_;

            if (over && have_room && settled) {
                for (int i = 0; i < step && buffer_.size() > 1; ++i) {
                    buffer_.pop_front();
                    drops_ += 1;
                }
                buffer_size_ = static_cast<int64_t>(buffer_.size());
                last_correction_ = ticks_;
                occupancy_avg_   = static_cast<double>(buffer_.size());
                buffer_cond_.notify_all();
                graph_->set_tag(diagnostics::tag_severity::INFO, "drop");
            }
        }

        if (format_desc_.field_count == 2) {
            // Check if the next frame is the correct 'field'
            auto is_field_1 = (buffer_[0].frame_count % 2) == 0;
            if ((field == core::video_field::a && !is_field_1) || (field == core::video_field::b && is_field_1)) {
                graph_->set_tag(diagnostics::tag_severity::WARNING, "underflow");
                latency_ += 1;
                return core::draw_frame{};
            }
        }

        if (latency_ != -1) {
            CASPAR_LOG(warning) << print() << " Latency: " << latency_;
            latency_ = -1;
        }

        frame_          = buffer_[0].frame;
        frame_time_     = buffer_[0].pts;
        frame_duration_ = buffer_[0].duration;
        frame_flush_    = false;

        buffer_.pop_front();
        buffer_size_ = static_cast<int64_t>(buffer_.size());
        buffer_cond_.notify_all();

        graph_->set_value("buffer", static_cast<double>(buffer_.size()) / static_cast<double>(buffer_capacity_));

        return frame_;
    }

    void seek(int64_t time)
    {
        CASPAR_SCOPE_EXIT { update_state(); };

        seek_ = av_rescale_q(time, format_tb_, TIME_BASE_Q);

        {
            boost::lock_guard<boost::mutex> lock(buffer_mutex_);
            buffer_.clear();
            buffer_size_ = 0;
            // The timeline restarts here, so the occupancy average must not carry the old
            // stream's level across -- it would make the governor correct for a condition that
            // no longer exists.
            occupancy_primed_ = false;
            occupancy_avg_    = 0.0;
            buffer_cond_.notify_all();
            graph_->set_value("buffer", static_cast<double>(buffer_.size()) / static_cast<double>(buffer_capacity_));
        }
    }

    int64_t time() const
    {
        if (frame_time_ == AV_NOPTS_VALUE) {
            // TODO (fix) How to handle NOPTS case?
            return 0;
        }

        return av_rescale_q(frame_time_, TIME_BASE_Q, format_tb_);
    }

    void loop(bool loop)
    {
        CASPAR_SCOPE_EXIT { update_state(); };

        loop_ = loop;
    }

    bool loop() const { return loop_; }

    void start(int64_t start)
    {
        CASPAR_SCOPE_EXIT { update_state(); };
        start_ = av_rescale_q(start, format_tb_, TIME_BASE_Q);
    }

    std::optional<int64_t> start() const
    {
        auto start = start_.load();
        return start != AV_NOPTS_VALUE ? av_rescale_q(start, TIME_BASE_Q, format_tb_) : std::optional<int64_t>();
    }

    void duration(int64_t duration)
    {
        CASPAR_SCOPE_EXIT { update_state(); };

        duration_ = av_rescale_q(duration, format_tb_, TIME_BASE_Q);
    }

    std::optional<int64_t> duration() const
    {
        const auto duration = duration_.load();
        if (duration == AV_NOPTS_VALUE) {
            return {};
        }
        return av_rescale_q(duration, TIME_BASE_Q, format_tb_);
    }

    std::optional<int64_t> file_duration() const
    {
        const auto input_duration = input_duration_.load();
        if (input_duration == AV_NOPTS_VALUE) {
            return {};
        }
        return av_rescale_q(input_duration, TIME_BASE_Q, format_tb_);
    }

  private:
    bool want_packet()
    {
        return std::any_of(decoders_.begin(), decoders_.end(), [](auto& p) { return p.second.want_packet(); });
    }

    bool schedule()
    {
        auto result = false;

        std::shared_ptr<AVPacket> packet;
        while (want_packet() && input_.try_pop(packet)) {
            result = true;

            if (!packet) {
                for (auto& p : decoders_) {
                    p.second.push(nullptr);
                }
            } else if (sources_.find(packet->stream_index) != sources_.end()) {
                auto it = decoders_.find(packet->stream_index);
                if (it != decoders_.end()) {
                    // TODO (fix): limit it->second.input.size()?
                    it->second.push(std::move(packet));
                }
            }
        }

        std::vector<int> eof;

        for (auto& p : sources_) {
            auto it = decoders_.find(p.first);
            if (it == decoders_.end()) {
                continue;
            }

            auto nb_requests = 0U;
            for (auto source : p.second) {
                nb_requests = std::max(nb_requests, av_buffersrc_get_nb_failed_requests(source));
            }

            if (nb_requests == 0) {
                continue;
            }

            auto frame = it->second.pop();
            if (!frame) {
                continue;
            }

            for (auto& source : p.second) {
                if (!frame->data[0]) {
                    FF(av_buffersrc_close(source, frame->pts, 0));
                } else {
                    // TODO (fix) Guard against overflow?
                    FF(av_buffersrc_write_frame(source, frame.get()));
                }
                result = true;
            }

            // End Of File
            if (!frame->data[0]) {
                eof.push_back(p.first);
            }
        }

        for (auto index : eof) {
            sources_.erase(index);
        }

        return result;
    }

    void seek_internal(int64_t time)
    {
        time = time != AV_NOPTS_VALUE ? time : 0;
        time = time + (input_->start_time != AV_NOPTS_VALUE ? input_->start_time : 0);

        // TODO (fix) Dont seek if time is close future.
        if (seekable_) {
            input_.seek(time);
        }
        frame_flush_ = true;
        frame_count_ = 0;
        buffer_eof_  = false;

        decoders_.clear();

        reset(time);
    }

    void reset(int64_t start_time)
    {
        video_filter_ = Filter(vfilter_, input_, decoders_, start_time, AVMEDIA_TYPE_VIDEO, format_desc_);
        audio_filter_ = Filter(afilter_, input_, decoders_, start_time, AVMEDIA_TYPE_AUDIO, format_desc_);

        sources_.clear();
        for (auto& p : video_filter_.sources) {
            sources_[p.first].push_back(p.second);
        }
        for (auto& p : audio_filter_.sources) {
            sources_[p.first].push_back(p.second);
        }

        std::vector<int> keys;
        // Flush unused inputs.
        for (auto& p : decoders_) {
            if (sources_.find(p.first) == sources_.end()) {
                keys.push_back(p.first);
            }
        }

        for (auto& key : keys) {
            decoders_.erase(key);
        }
    }

    std::string print() const
    {
        const int          position = std::max(static_cast<int>(time() - start().value_or(0)), 0);
        std::ostringstream str;
        str << std::fixed << std::setprecision(4) << "ffmpeg[" << name_ << "|"
            << av_q2d({position * format_tb_.num, format_tb_.den}) << "/"
            << av_q2d({static_cast<int>(duration().value_or(0LL)) * format_tb_.num, format_tb_.den}) << "]";
        return str.str();
    }
};

AVProducer::AVProducer(std::shared_ptr<core::frame_factory> frame_factory,
                       core::video_format_desc              format_desc,
                       std::string                          name,
                       std::string                          path,
                       std::optional<std::string>           vfilter,
                       std::optional<std::string>           afilter,
                       std::optional<int64_t>               start,
                       std::optional<int64_t>               seek,
                       std::optional<int64_t>               duration,
                       std::optional<bool>                  loop,
                       int                                  seekable,
                       core::frame_geometry::scale_mode     scale_mode)
    : impl_(new Impl(std::move(frame_factory),
                     std::move(format_desc),
                     std::move(name),
                     std::move(path),
                     std::move(vfilter.value_or("")),
                     std::move(afilter.value_or("")),
                     std::move(start),
                     std::move(seek),
                     std::move(duration),
                     std::move(loop.value_or(false)),
                     seekable,
                     scale_mode))
{
}

core::draw_frame AVProducer::next_frame(const core::video_field field) { return impl_->next_frame(field); }

core::draw_frame AVProducer::prev_frame(const core::video_field field) { return impl_->prev_frame(field); }

bool AVProducer::is_ready() { return impl_->is_ready(); }

AVProducer& AVProducer::seek(int64_t time)
{
    impl_->seek(time);
    return *this;
}

AVProducer& AVProducer::loop(bool loop)
{
    impl_->loop(loop);
    return *this;
}

bool AVProducer::loop() const { return impl_->loop(); }

AVProducer& AVProducer::start(int64_t start)
{
    impl_->start(start);
    return *this;
}

int64_t AVProducer::time() const { return impl_->time(); }

int64_t AVProducer::start() const { return impl_->start().value_or(0); }

AVProducer& AVProducer::duration(int64_t duration)
{
    impl_->duration(duration);
    return *this;
}

int64_t AVProducer::duration() const { return impl_->duration().value_or(std::numeric_limits<int64_t>::max()); }

core::monitor::state AVProducer::state() const
{
    boost::lock_guard<boost::mutex> lock(impl_->state_mutex_);
    return impl_->state_;
}

}} // namespace caspar::ffmpeg
