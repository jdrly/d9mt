#pragma once

// D9MT_LATENCY_LOG=1 (or =<file>): one line per frame, from the app's Present call
// to the GPU finishing the frame's last command buffer:
//   f=<frame> t=<s since the first frame> cs=+ acq=+ sub0=+ sub=+ pres=+ gpu=+ (ms after Present) cb=<n>
//     cs    the CS thread starts the present (its backlog)
//     acq   the drawable is acquired (cs..acq: waiting for a drawable)
//     sub0  the frame's first command buffer is committed (the GPU can start)
//     sub   the last one (with the present copy) is committed
//     pres  the present is scheduled (MetalDrawable_present returned; not on-glass time,
//           which needs the drawable's presented handler in the bridge); "skip" for a frame
//           not presented (hidden/occluded window, no drawable: acq=sub= the skip's flush)
//     gpu   the GPU has finished the last command buffer (the shared event reached its
//           serial, seen by the log's own thread); "gpu<=" when the thread was late, an upper bound
// Every 256 frames a LATENCY summary (p50/p99) goes to the regular log. The file defaults to
// d9mt-latency.log in the working directory. Only the log's thread does file I/O and waits.

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "wmt_handle.h"

#include "../util/thread.h"

namespace dxvk::backend {

  struct FrameLatencyRecord {
    uint64_t frameId   = 0u;
    uint64_t serial    = 0u;   // of the frame's last command buffer
    uint64_t appNs     = 0u;
    uint64_t csNs      = 0u;
    uint64_t acquireNs = 0u;
    uint64_t submit0Ns = 0u;
    uint64_t submitNs  = 0u;
    uint64_t presentNs = 0u;
    uint32_t cmdbufs   = 0u;
  };

  class FrameLatencyLog {

  public:

    FrameLatencyLog(obj_handle_t event, const char* path);

    ~FrameLatencyLog();

    // CS thread, after the present: queued for the log's thread.
    void push(const FrameLatencyRecord& r);

  private:

    obj_handle_t            m_event;
    std::FILE*              m_file = nullptr;
    std::mutex              m_mutex;
    std::condition_variable m_cond;
    std::deque<FrameLatencyRecord> m_queue;
    bool                    m_stop = false;
    uint64_t                m_t0   = 0u;
    uint32_t                m_unflushed = 0u;

    // ms after Present: app->sub, app->gpu, sub->gpu, for the summary
    std::vector<double>     m_sub, m_gpu, m_subGpu;

    dxvk::thread            m_thread;

    void run();
    void write(const FrameLatencyRecord& r, uint64_t gpuNs, bool late);
    void summary(uint64_t frameId);

  };

}
