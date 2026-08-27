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

    AVFormatContext* operator->();

    AVFormatContext* const operator->() const;

    void reset();
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
    std::condition_variable          ic_cond_;

    tbb::concurrent_bounded_queue<std::shared_ptr<AVPacket>> buffer_;

    std::atomic<bool> eof_{false};

    std::atomic<bool> abort_request_{false};
    boost::thread     thread_;
};

}} // namespace caspar::ffmpeg
