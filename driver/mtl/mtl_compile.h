#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>

#include "../util/thread.h"
#include "../util/util_env.h"
#include "../util/log/log.h"
#include "../util/util_string.h"
#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk {

  /**
   * \brief Run-once compile job (shader codegen, PSO build)
   *
   * Runs on a compile worker, or inline on the first thread that needs the
   * result while it is still queued, so a waiter never sits behind unrelated
   * prefetch work. run() publishes its results by returning: everything it
   * wrote is visible to any thread that observed isDone() or left wait().
   */
  class MtlCompileTask : public RcObject {

  public:

    virtual ~MtlCompileTask() { }

    bool isDone() const {
      return m_state.load(std::memory_order_acquire) == Done;
    }

    /// Runs the task if nobody has claimed it yet. True if it ran here.
    bool tryRun() {
      uint32_t expected = Pending;

      if (!m_state.compare_exchange_strong(expected, Running, std::memory_order_acq_rel))
        return false;

      try {
        run();
      } catch (const std::exception& e) {
        Logger::err(str::format("MtlCompileTask: ", e.what()));
      } catch (...) {
        Logger::err("MtlCompileTask: unknown exception");
      }

      { std::lock_guard lock(m_mutex);
        m_state.store(Done, std::memory_order_release);
      }

      m_cond.notify_all();
      return true;
    }

    void wait() {
      if (isDone() || tryRun())
        return;

      std::unique_lock lock(m_mutex);
      m_cond.wait(lock, [this] { return isDone(); });
    }

  protected:

    virtual void run() = 0;

  private:

    enum : uint32_t { Pending, Running, Done };

    std::atomic<uint32_t>     m_state = { Pending };
    dxvk::mutex               m_mutex;
    dxvk::condition_variable  m_cond;

  };


  /**
   * \brief Compile worker pool
   *
   * Threads start on demand up to the cap; 0 threads = submit() is a no-op
   * and tasks run on their first wait(). Destruction drops queued tasks and
   * joins, so a task never runs after its owner (the device) is gone and may
   * hold raw device/compiler handles.
   */
  class MtlCompileWorkers {

  public:

    explicit MtlCompileWorkers(uint32_t maxThreads)
    : m_maxThreads(maxThreads) { }

    ~MtlCompileWorkers() {
      std::deque<Rc<MtlCompileTask>> dropped[2];

      { std::lock_guard lock(m_mutex);
        m_stop = true;
        dropped[0].swap(m_queue[0]);
        dropped[1].swap(m_queue[1]);
      }

      m_cond.notify_all();

      for (auto& t : m_threads)
        t.join();
    }

    /// Urgent work (a draw needs it) runs before prefetch work; FIFO within
    /// each class.
    void submit(const Rc<MtlCompileTask>& task, bool urgent) {
      if (!m_maxThreads || task == nullptr || task->isDone())
        return;

      { std::lock_guard lock(m_mutex);
        if (m_stop)
          return;

        m_queue[urgent ? 0 : 1].push_back(task);

        if (m_threads.size() < m_maxThreads)
          m_threads.emplace_back([this] { loop(); });
      }

      m_cond.notify_one();
    }

    size_t pending() {
      std::lock_guard lock(m_mutex);
      return m_queue[0].size() + m_queue[1].size();
    }

    uint32_t maxThreads() const {
      return m_maxThreads;
    }

  private:

    dxvk::mutex               m_mutex;
    dxvk::condition_variable  m_cond;
    std::deque<Rc<MtlCompileTask>> m_queue[2];   // urgent, prefetch
    std::vector<dxvk::thread> m_threads;
    uint32_t                  m_maxThreads = 0u;
    bool                      m_stop = false;

    void loop() {
#ifdef _WIN32
      env::setThreadName("d9mt-compile");

      // Prefetch work runs at lowest priority, urgent work at normal
      // priority so the render thread never waits on a deprioritized thread.
      bool urgent = true;
#endif

      while (true) {
        Rc<MtlCompileTask> task;
        bool isUrgent;

        { std::unique_lock lock(m_mutex);
          m_cond.wait(lock, [this] { return m_stop || !m_queue[0].empty() || !m_queue[1].empty(); });

          if (m_stop)
            return;

          isUrgent = !m_queue[0].empty();
          auto& q = m_queue[isUrgent ? 0 : 1];
          task = std::move(q.front());
          q.pop_front();
        }

        if (task->isDone())
          continue;

#ifdef _WIN32
        if (isUrgent != urgent) {
          urgent = isUrgent;
          ::SetThreadPriority(::GetCurrentThread(),
            urgent ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_LOWEST);
        }
#endif

        // No-op if a waiter already ran it inline.
        task->tryRun();
      }
    }

  };


  /**
   * \brief D9MT_PROFILE compile counters
   *
   * Bumped from workers and the CS thread; one COMPILE line per frame that
   * had compile activity or took longer than 33 ms. Zero cost without
   * D9MT_PROFILE beyond a branch.
   */
  namespace compile_stats {

    inline const bool g_enabled = std::getenv("D9MT_PROFILE") != nullptr;

    struct Counter {
      std::atomic<uint64_t> n  = { 0u };
      std::atomic<uint64_t> ns = { 0u };
      // Never reset: the app-thread HITCH line takes per-frame deltas.
      std::atomic<uint64_t> totalN  = { 0u };
      std::atomic<uint64_t> totalNs = { 0u };

      void add(uint64_t dns) {
        n.fetch_add(1u, std::memory_order_relaxed);
        ns.fetch_add(dns, std::memory_order_relaxed);
        totalN.fetch_add(1u, std::memory_order_relaxed);
        totalNs.fetch_add(dns, std::memory_order_relaxed);
      }
    };

    /// D9MT_HITCH_MS (default 50): frames at least this long log at warn level.
    inline double hitchMs() {
      static const double s_ms = [] {
        const char* v = std::getenv("D9MT_HITCH_MS");
        double ms = v ? std::strtod(v, nullptr) : 0.0;
        return ms > 0.0 ? ms : 50.0;
      } ();
      return s_ms;
    }

    inline Counter g_shaders;   // AIR codegen + MTLLibrary (worker or inline)
    inline Counter g_shared;    // linkages that reused an identical library
    inline Counter g_psos;      // render PSO builds
    inline Counter g_wait;      // render (CS) thread blocked on a compile
    inline Counter g_skips;     // draws dropped by D9MT_ASYNC_SKIP

    inline uint64_t nowNs() {
      return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    /// Waits for a task on the render thread, accounting the blocked time.
    inline void waitOnRenderThread(MtlCompileTask* task) {
      if (task == nullptr || task->isDone())
        return;

      if (!g_enabled) {
        task->wait();
        return;
      }

      uint64_t t0 = nowNs();
      task->wait();
      g_wait.add(nowNs() - t0);
    }

    inline void onFrameEnd(size_t queued) {
      if (!g_enabled)
        return;

      static uint64_t s_frame = 0u;
      static uint64_t s_last = 0u;

      uint64_t now = nowNs();
      double dtMs = s_last ? double(now - s_last) * 1e-6 : 0.0;
      s_last = now;
      s_frame++;

      auto take = [] (Counter& c, uint64_t& n) {
        n = c.n.exchange(0u, std::memory_order_relaxed);
        return double(c.ns.exchange(0u, std::memory_order_relaxed)) * 1e-6;
      };

      uint64_t nSh, nShared, nPso, nWait, nSkip;
      double msSh = take(g_shaders, nSh);
      take(g_shared, nShared);
      double msPso = take(g_psos, nPso);
      double msWait = take(g_wait, nWait);
      take(g_skips, nSkip);

      if (!(nSh | nShared | nPso | nWait | nSkip) && dtMs < 33.4)
        return;

      // Render-thread frames: a long one is part of a hitch, so make it
      // visible at DXVK_LOG_LEVEL=warn next to the app-thread HITCH line.
      std::string line = str::format("COMPILE frame=", s_frame, " dtMs=", dtMs,
        " shaders=", nSh, "/", msSh, "ms shared=", nShared,
        " psos=", nPso, "/", msPso, "ms renderWait=", nWait, "/", msWait,
        "ms skipped=", nSkip, " queued=", queued);
      if (dtMs >= hitchMs())
        Logger::warn(line);
      else
        Logger::info(line);
    }

  }

}
