#include "av_input.h"

#include "../util/av_assert.h"
#include "../util/av_util.h"

#include <common/env.h>
#include <common/except.h>
#include <common/os/thread.h>
#include <common/param.h>
#include <common/scope_exit.h>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <set>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#include <libavformat/avformat.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace caspar { namespace ffmpeg {

Input::Input(const std::string& filename, std::shared_ptr<diagnostics::graph> graph, std::optional<bool> seekable)
    : filename_(filename)
    , graph_(graph)
    , seekable_(seekable)
{
    graph_->set_color("seek", diagnostics::color(1.0f, 0.5f, 0.0f));
    graph_->set_color("input", diagnostics::color(0.7f, 0.4f, 0.4f));

    buffer_.set_capacity(256);
    thread_ = boost::thread([=] { run_read_loop(); });
}

bool Input::is_live() const
{
    // Protocols with no end and no seek: losing one is a network event, not a fault, and the
    // only useful response is to open it again. A file behaves oppositely -- a read error there
    // is real, and reopening in a loop would spin forever on a bad disk.
    static const std::set<std::wstring> LIVE_PROTOCOLS = {
        L"srt", L"udp", L"rtp", L"rtsp", L"rtmp", L"rtmps", L"http", L"https", L"tcp"};

    const auto scheme = caspar::protocol_split(u16(filename_)).first;
    return LIVE_PROTOCOLS.find(boost::to_lower_copy(scheme)) != LIVE_PROTOCOLS.end();
}

void Input::run_read_loop()
{
    set_thread_name(L"[ffmpeg::av_producer::Input]");

    // ── The try/catch is INSIDE the loop, and that is the entire point ──────────────────
    // It used to wrap the whole `while (true)`, so `FF_RET(ret, "av_read_frame")` throwing --
    // which it does for ANY read error, including a transient one -- unwound past the loop and
    // ended the thread. There is no supervisor above it, so the input was then dead for the
    // process's lifetime: no packets, no EOF, no error after the first. The producer simply
    // logged "Waiting for frame..." forever and the layer froze on its last picture.
    //
    // On a 24/7 wall fed by SRT that is not an edge case. `rw_timeout` is 60 s, so a minute of
    // silence from one encoder retires that tile permanently.
    int  backoff_ms   = 0;
    bool was_reopened = false;

    while (!abort_request_) {
        try {
            auto packet = alloc_packet();

            {
                std::unique_lock<std::mutex> lock(ic_mutex_);
                ic_cond_.wait(lock, [&] { return ic_ || abort_request_; });

                if (abort_request_) {
                    break;
                }

                if (was_reopened) {
                    // Announced only once the reopen has actually produced a context, so a
                    // stream that reconnects but never delivers does not claim it recovered.
                    was_reopened   = false;
                    discontinuity_ = true;
                }

                // TODO (perf) Non blocking av_read_frame when possible.
                auto ret = av_read_frame(ic_.get(), packet.get());

                if (ret == AVERROR_EXIT) {
                    break;
                } else if (ret == AVERROR(EAGAIN)) {
                    boost::this_thread::yield();
                } else if (ret == AVERROR_EOF) {
                    eof_   = true;
                    packet = nullptr;
                } else {
                    FF_RET(ret, "av_read_frame");
                }
            }

            backoff_ms = 0; // a good read clears the penalty
            buffer_.push(std::move(packet));
            graph_->set_value("input", (static_cast<double>(buffer_.size()) / buffer_.capacity()));
        } catch (...) {
            if (abort_request_) {
                break;
            }

            if (!is_live()) {
                // A file read error is a real error. Preserve the old behaviour exactly:
                // report it and stop, rather than reopening a broken file forever.
                CASPAR_LOG_CURRENT_EXCEPTION();
                break;
            }

            // Exponential backoff to 5 s. Twenty tiles all retrying a dead switch at frame rate
            // would be its own outage.
            backoff_ms = backoff_ms == 0 ? 100 : std::min(backoff_ms * 2, 5000);

            const auto attempt = reconnects_.fetch_add(1) + 1;
            if (attempt == 1 || attempt % 20 == 0) {
                // Latched: once when it starts, then rarely. A per-attempt log across twenty
                // layers buries everything else in the file.
                CASPAR_LOG(warning) << "av_input[" + filename_ + "] read failed; reopening (attempt "
                                    << attempt << ", backoff " << backoff_ms << " ms)";
            }

            boost::this_thread::sleep_for(boost::chrono::milliseconds(backoff_ms));
            if (abort_request_) {
                break;
            }

            try {
                std::unique_lock<std::mutex> lock(ic_mutex_);
                eof_ = false;
                internal_reset();
                was_reopened = true;
            } catch (...) {
                // Still down. Stay in the loop and try again after a longer backoff; do not
                // log again here, the counter above already tells the story.
            }
        }
    }
}

Input::~Input()
{
    graph_         = spl::shared_ptr<diagnostics::graph>();
    abort_request_ = true;
    ic_cond_.notify_all();

    std::shared_ptr<AVPacket> packet;
    while (buffer_.try_pop(packet))
        ;

    thread_.join();
}

int Input::interrupt_cb(void* ctx)
{
    auto input = reinterpret_cast<Input*>(ctx);
    return input->abort_request_ ? 1 : 0;
}

bool Input::try_pop(std::shared_ptr<AVPacket>& packet)
{
    auto result = buffer_.try_pop(packet);
    graph_->set_value("input", (static_cast<double>(buffer_.size()) / buffer_.capacity()));
    return result;
}

AVFormatContext*       Input::operator->() { return ic_.get(); }
AVFormatContext* const Input::operator->() const { return ic_.get(); }

void Input::abort()
{
    abort_request_ = true;
    ic_cond_.notify_all();

    std::shared_ptr<AVPacket> packet;
    while (buffer_.try_pop(packet))
        ;
}

void Input::reset()
{
    std::unique_lock<std::mutex> lock(ic_mutex_);
    internal_reset();
}

void Input::internal_reset()
{
    AVDictionary* options = nullptr;
    CASPAR_SCOPE_EXIT { av_dict_free(&options); };

    static const std::set<std::wstring> PROTOCOLS_TREATED_AS_FORMATS = {L"dshow", L"v4l2", L"iec61883"};

#if LIBAVFORMAT_VERSION_MAJOR >= 59
    const AVInputFormat* input_format = nullptr;
#else
    AVInputFormat* input_format = nullptr;
#endif
    auto url_parts = caspar::protocol_split(u16(filename_));
    if (url_parts.first == L"http" || url_parts.first == L"https") {
        FF(av_dict_set(&options, "multiple_requests", "1", 0)); // NOTE https://trac.ffmpeg.org/ticket/7034#comment:3
        FF(av_dict_set(&options, "reconnect", "1", 0));
        FF(av_dict_set(&options, "reconnect_streamed", "1", 0));
        FF(av_dict_set(&options, "reconnect_delay_max", "120", 0));
        FF(av_dict_set(&options, "referer", filename_.c_str(), 0)); // HTTP referer header
    } else if (url_parts.first == L"rtmp" || url_parts.first == L"rtmps") {
        FF(av_dict_set(&options, "rtmp_live", "live", 0));
    } else if (PROTOCOLS_TREATED_AS_FORMATS.find(url_parts.first) != PROTOCOLS_TREATED_AS_FORMATS.end()) {
        input_format = av_find_input_format(u8(url_parts.first).c_str());
        filename_    = u8(url_parts.second);
    }

    if (seekable_) {
        CASPAR_LOG(debug) << "av_input[" + filename_ + "] Disabled seeking";
        FF(av_dict_set(&options, "seekable", *seekable_ ? "1" : "0", 0));
    }

    if (input_format == nullptr) {
        // TODO (fix) timeout?
        FF(av_dict_set(&options, "rw_timeout", "60000000", 0)); // 60 second IO timeout
    }

    // ── A common epoch for feeds that have none ─────────────────────────────────────────
    // Twenty encoders that are not PTP-locked produce twenty unrelated PTS epochs, so a
    // timestamp cannot say two feeds were captured at the same instant -- only that one feed is
    // consistent with itself. `use_wallclock_as_timestamps` replaces the PTS with this machine's
    // clock at the moment the packet arrives (`demux.c:565`), which gives every feed one epoch
    // and, incidentally, makes PTS wraparound irrelevant.
    //
    // What it costs is worth stating plainly: it measures ARRIVAL, not capture. Network jitter
    // becomes timing jitter, and a feed that traverses a worse path reads as permanently later.
    // For a monitoring wall whose requirement is "no feed accumulates error" that is a good
    // trade; for anything where absolute capture time matters it is the wrong tool.
    //
    // Live only, and off by default. On a file it would stamp at read speed rather than playback
    // rate, which is meaningless -- so the guard is correctness, not caution.
    if (is_live() && env::properties().get(L"configuration.ffmpeg.producer.sync.wallclock-timestamps", false)) {
        FF(av_dict_set(&options, "use_wallclock_as_timestamps", "1", 0));
    }

    AVFormatContext* ic             = avformat_alloc_context();
    ic->interrupt_callback.callback = Input::interrupt_cb;
    ic->interrupt_callback.opaque   = this;

    FF(avformat_open_input(&ic, filename_.c_str(), input_format, &options));
    auto ic2 = std::shared_ptr<AVFormatContext>(ic, [](AVFormatContext* ctx) { avformat_close_input(&ctx); });

    for (auto& p : to_map(&options)) {
        CASPAR_LOG(warning) << "av_input[" + filename_ + "]" << " Unused option " << p.first << "=" << p.second;
    }

    FF(avformat_find_stream_info(ic2.get(), nullptr));
    ic_ = std::move(ic2);
    ic_cond_.notify_all();
}

bool Input::eof() const { return eof_; }

void Input::seek(int64_t ts, bool flush)
{
    std::unique_lock<std::mutex> lock(ic_mutex_);

    if (ic_ && ts != ic_->start_time && ts != AV_NOPTS_VALUE) {
        FF(avformat_seek_file(ic_.get(), -1, INT64_MIN, ts, ts, 0));
    } else {
        internal_reset();
    }

    if (flush) {
        std::shared_ptr<AVPacket> packet;
        while (buffer_.try_pop(packet))
            ;
    }
    eof_ = false;

    graph_->set_tag(diagnostics::tag_severity::INFO, "seek");
}

}} // namespace caspar::ffmpeg
