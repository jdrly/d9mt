#include "mtl_latency_log.h"

#include <algorithm>
#include <cstring>

#include "../mtl/mtl_latency.h"
#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

namespace dxvk::backend {

  static double ms(uint64_t from, uint64_t to) {
    return to >= from ? double(to - from) * 1e-6 : -double(from - to) * 1e-6;
  }

  FrameLatencyLog::FrameLatencyLog(obj_handle_t event, const char* path)
  : m_event(event) {
    std::string file = path && *path && std::strcmp(path, "1") && std::strcmp(path, "true")
      ? std::string(path) : std::string("d9mt-latency.log");
    m_file = std::fopen(file.c_str(), "w");
    if (!m_file) {
      Logger::warn(str::format("D9MT_LATENCY_LOG: cannot open ", file, ": latency log off"));
      return;
    }
    std::setvbuf(m_file, nullptr, _IOFBF, 1u << 16);
    std::fprintf(m_file, "# d9mt frame latency: ms after the app's Present; cs=CS starts the present,"
      " acq=drawable acquired, sub0/sub=first/last command buffer committed, pres=present scheduled,"
      " gpu=GPU done with the last command buffer (gpu<= : upper bound), cb=command buffers\n");
    Logger::info(str::format("D9MT_LATENCY_LOG: per-frame latency -> ", file));
    m_sub.reserve(256u); m_gpu.reserve(256u); m_subGpu.reserve(256u);
    m_thread = dxvk::thread([this] { run(); });
  }

  FrameLatencyLog::~FrameLatencyLog() {
    { std::lock_guard<std::mutex> lock(m_mutex);
      m_stop = true;
    }
    m_cond.notify_one();
    if (m_thread.joinable())
      m_thread.join();
    if (m_file)
      std::fclose(m_file);
  }

  void FrameLatencyLog::push(const FrameLatencyRecord& r) {
    if (!m_file)
      return;
    { std::lock_guard<std::mutex> lock(m_mutex);
      // A stuck GPU must not grow this without bound: drop the oldest.
      if (m_queue.size() >= 1024u)
        m_queue.pop_front();
      m_queue.push_back(r);
    }
    m_cond.notify_one();
  }

  void FrameLatencyLog::run() {
    env::setThreadName("d9mt-latency");
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
      m_cond.wait(lock, [this] { return m_stop || !m_queue.empty(); });
      if (m_queue.empty())
        return;
      FrameLatencyRecord r = m_queue.front();
      m_queue.pop_front();
      lock.unlock();

      // Frames complete in order and this thread waits on the oldest one, so the wake-up is
      // the completion time unless it was already past when the wait started.
      bool late = MTLSharedEvent_signaledValue(m_event) >= r.serial;
      bool done = late;
      while (!done) {
        done = MTLSharedEvent_waitUntilSignaledValue(m_event, r.serial, 100u);
        if (!done) {
          std::lock_guard<std::mutex> l(m_mutex);
          if (m_stop)
            break;
        }
      }
      if (done)
        write(r, latency::nowNs(), late);

      lock.lock();
    }
  }

  void FrameLatencyLog::write(const FrameLatencyRecord& r, uint64_t gpuNs, bool late) {
    if (!m_t0)
      m_t0 = r.appNs;
    const uint64_t a = r.appNs;
    char pres[32];
    if (r.presentNs)
      std::snprintf(pres, sizeof(pres), "%.3f", ms(a, r.presentNs));
    else
      std::snprintf(pres, sizeof(pres), "skip");   // not presented: hidden window or no drawable
    std::fprintf(m_file, "f=%llu t=%.3f cs=%.3f acq=%.3f sub0=%.3f sub=%.3f pres=%s gpu%s%.3f cb=%u\n",
      (unsigned long long)r.frameId, double(a - m_t0) * 1e-9,
      ms(a, r.csNs), ms(a, r.acquireNs), ms(a, r.submit0Ns ? r.submit0Ns : r.submitNs),
      ms(a, r.submitNs), pres, late ? "<=" : "=", ms(a, gpuNs), r.cmdbufs);
    if (++m_unflushed >= 64u) {
      std::fflush(m_file);
      m_unflushed = 0u;
    }

    m_sub.push_back(ms(a, r.submitNs));
    m_gpu.push_back(ms(a, gpuNs));
    m_subGpu.push_back(ms(r.submitNs, gpuNs));
    if (m_sub.size() >= 256u)
      summary(r.frameId);
  }

  void FrameLatencyLog::summary(uint64_t frameId) {
    auto pct = [](std::vector<double>& v, double p) {
      size_t i = std::min(v.size() - 1u, size_t(double(v.size()) * p));
      std::nth_element(v.begin(), v.begin() + i, v.end());
      return v[i];
    };
    Logger::info(str::format("LATENCY frame=", frameId, " n=", m_sub.size(),
      " ms p50/p99: present->submit=", pct(m_sub, 0.5), "/", pct(m_sub, 0.99),
      " present->gpuDone=", pct(m_gpu, 0.5), "/", pct(m_gpu, 0.99),
      " submit->gpuDone=", pct(m_subGpu, 0.5), "/", pct(m_subGpu, 0.99)));
    m_sub.clear(); m_gpu.clear(); m_subGpu.clear();
  }

}
