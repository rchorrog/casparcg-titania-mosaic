#pragma once

#include <common/diagnostics/graph.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>

#include <tbb/concurrent_queue.h>

#include <boost/thread.hpp>

struct AVPacket;
struct AVFormatContext;

namespace caspar { namespace ffmpeg {

class Input
{
  public:
    Input(const std::string& filename, std::shared_ptr<diagnostics::graph> graph, std::optional<bool> seekable);
    ~Input();

    static int interrupt_cb(void* ctx);

    bool try_pop(std::shared_ptr<AVPacket>& packet);

    /// The current format context, OWNED by the caller for as long as it holds the pointer.
    ///
    /// A live input's read thread replaces the context on every reconnect (`internal_reset`),
    /// and the old one is freed when its last owner lets go. Until 2026-11 these returned
    /// `ic_.get()` with no lock, so the producer thread could read a context the read thread was
    /// freeing. `input->field` still works: the returned shared_ptr lives until the end of the
    /// full expression. Anything that keeps a pointer INTO the context (an AVStream*) must keep
    /// the shared_ptr too -- see `context()` and `Decoder::owner`.
    ///
    /// Never takes `ic_mutex_`: the read thread holds that across a blocking av_read_frame (up
    /// to rw_timeout), and the producer thread must not wait on the network.
    std::shared_ptr<AVFormatContext> operator->() const { return context(); }
    std::shared_ptr<AVFormatContext> context() const;

    void reset();
    /// First open, from the producer thread. Like `reset`, but a live input that fails to open
    /// is retried until it opens or the producer is destroyed. See `Input::open`.
    void open();
    void abort();
    bool eof() const;
    void seek(int64_t ts, bool flush = true);

    /// How many times the read thread has reopened this input after a failure.
    ///
    /// Live only: a file that fails to read is a real error, and reconnecting to one would
    /// loop forever on a bad disk. See `is_live_` and `run_read_loop`.
    int64_t reconnects() const { return reconnects_.load(); }

    /// True once since the last call: the stream restarted, so anything downstream that
    /// assumes a continuous timeline -- the filter graph in particular -- has to be rebuilt.
    /// `vf_fps` never re-seeds its output counter on its own, so without this a reconnected
    /// stream makes it drop or duplicate every frame indefinitely.
    bool take_discontinuity() { return discontinuity_.exchange(false); }

  private:
    void internal_reset();
    void run_read_loop();
    bool is_live() const;

    std::atomic<int64_t> reconnects_{0};
    std::atomic<bool>    discontinuity_{false};

    std::optional<bool> seekable_;

    std::string                         filename_;
    std::shared_ptr<diagnostics::graph> graph_;

    mutable std::mutex               ic_mutex_;
    std::shared_ptr<AVFormatContext> ic_;

    /// `ic_` as published to other threads, under a lock held only to copy the pointer.
    mutable std::mutex               published_mutex_;
    std::shared_ptr<AVFormatContext> published_;
    std::condition_variable          ic_cond_;

    tbb::concurrent_bounded_queue<std::shared_ptr<AVPacket>> buffer_;

    std::atomic<bool> eof_{false};

    std::atomic<bool> abort_request_{false};
    boost::thread     thread_;
};

}} // namespace caspar::ffmpeg
