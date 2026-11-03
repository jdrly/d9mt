#pragma once

// D9MT_PROFILE: the CS handoff between the game thread and the CS thread (d9mt #127, lane OPT's
// R1b). One PROF-CS line follows every PROF line (every 256 frames, on the CS thread):
//
//   PROF-CS frame=512 win=256 per-frame: csChunk=61.2 csWake=40.1 csSyncWait=0.02
//     csSyncWaitMs=0.001 | csRun=61.2 csKiB=930.5 csPark=40.1 csParkMs=12.40 csBusyMs=4.27
//     csSyncWake=0.02 | cmd=21890.1 cmdDraw=5410.0 cmdVb=7811.2 cmdIb=1203.4 cmdConst=5590.3
//     cmdTex=812.0 cmdShader=301.2 cmdState=640.0 cmdProf=118.0 cmdOther=4.0
//
// Every value is per frame (the window's total / 256). Keys are unique on the line; besides the
// window's frame= and win=, none is a key of the PROF line or of any other line, so parsers that
// read `key=value` pairs can't mix them up.
//
// The window is PROF's: it ends inside the CS command that runs the 256th EndFrame.
//
// Producer side (the threads that hand chunks to the CS thread; for SWTOR the game thread). Each
// event updates its fields together under the producer lock, and the window takes them under the
// same lock, so an event lands whole in one window (csWake <= csChunk always):
//   csChunk       chunks handed over (DxvkCsThread::dispatchChunk and injectChunk)
//   csWake        handovers that found the CS thread parked on its queue's condition variable:
//                 the notify had a sleeper to wake (under Wine, the thread-alert path). Counted
//                 once per park, however many chunks arrive before the CS thread runs again.
//   csSyncWait    waits for the CS thread to reach a chunk (synchronize(), injectChunk(..., true))
//                 that actually blocked: a call whose chunk was already done doesn't count
//   csSyncWaitMs  their time, ms per frame
// Consumer side (the CS thread; it also ends the window, so its counts need no lock):
//   csRun         chunks executed, each in the window where its first command runs
//   csKiB         those chunks' bytes (commands and their data), KiB per frame, same window
//   csPark        times the CS thread found its queues empty and slept on the condition variable
//   csParkMs      time parked, ms per frame
//   csBusyMs      the window's wall time minus csParkMs, ms per frame: everything the CS thread
//                 did while not parked, including waits inside commands (syncWsi, GPU waits;
//                 PROF's buckets break part of it down) and time it was runnable but descheduled.
//                 Wall time, not CPU time. Exact for one device: the CS thread writes the line
//                 itself, so it is never parked at a window's end.
//   csSyncWake    completed chunks whose notify found a thread inside a csSyncWait
// Commands executed, by kind (DxvkCsCmdKind: a template argument at the emit site, EmitCsAs), each
// counted right before it runs: the command that ends the window is in it, the next is not.
//   cmd           all commands
//   cmdDraw       draw commands; consecutive draws merged into one command count once (PROF's
//                 draws= counts each draw)
//   cmdVb cmdIb   vertex and index buffer binds and unbinds
//   cmdConst      constant uploads and binds (constant ring ranges, push data, spec constants)
//   cmdTex        texture and sampler binds and unbinds
//   cmdShader     shader, input layout and FF/linkage binds
//   cmdState      render state: framebuffer, viewport/scissor, blend, depth/stencil, raster,
//                 multisample, primitive type
//   cmdProf       ProfApi's API tags: commands D9MT_PROFILE itself adds (subtract for the
//                 profile-off count)
//   cmdOther      everything untagged: clears, copies, uploads, flushes, queries, frame markers,
//                 the texture initializer's commands
//
// Several devices: the producer record stays coherent, but the consumer counts have one writer per
// device's CS thread (cheap single-writer increments), so two CS threads can lose counts, and
// park times add up over the threads (busy clamps at 0). SWTOR has one device.
//
// Off (D9MT_PROFILE unset): the kind is a template argument of the command class
// (DxvkCsTypedCmd<T, K>), so a tagged emit builds and moves the lambda exactly as an untagged
// one, chunks pack the same bytes, and executeAll runs its original loop. What's left is one
// not-taken branch on g_enabled at each: chunk handover, chunk executed, CS-thread park, sync wait
// (synchronize / injectChunk with synchronize), sequenced chunk completion (next to its
// notify_all), and CS-thread startup. Never per command.
//
// Dependency-free so it builds natively (test/native/cs_stats_test.cpp).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../util/util_cheap_clock.h"

namespace dxvk {

  enum class DxvkCsCmdKind : uint8_t {
    Other = 0,
    Draw,
    VertexBuffer,
    IndexBuffer,
    Constants,
    Texture,
    Shader,
    State,
    Prof,
    Count
  };

}

namespace dxvk::cs_stats {

  inline const bool g_enabled = std::getenv("D9MT_PROFILE") != nullptr;

  constexpr uint32_t kKinds = uint32_t(DxvkCsCmdKind::Count);

  // A tiny lock for the producer record: held for a few additions or one copy-and-reset, only
  // with profiling on. Dependency-free (no dxvk::mutex here; std::mutex isn't one the PE build
  // uses).
  class SpinLock {
  public:
    void lock() {
      while (m_held.exchange(true, std::memory_order_acquire)) {
        while (m_held.load(std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(__i386__)
          __builtin_ia32_pause();
#endif
        }
      }
    }
    void unlock() {
      m_held.store(false, std::memory_order_release);
    }
  private:
    std::atomic<bool> m_held = { false };
  };

  struct ProducerCounts {
    uint64_t chunks        = 0u;
    uint64_t wakes         = 0u;
    uint64_t syncWaits     = 0u;
    uint64_t syncWaitTicks = 0u;
  };

  // One window's raw totals.
  struct Snapshot {
    ProducerCounts producer;
    uint64_t runs       = 0u;
    uint64_t bytes      = 0u;
    uint64_t parks      = 0u;
    uint64_t parkTicks  = 0u;
    uint64_t syncWakes  = 0u;
    uint64_t cmds[kKinds] = { };
    uint64_t wallTicks  = 0u;   // 0 if the window never started
  };

  struct Stats {
    // producer side: under producerLock
    SpinLock       producerLock;
    ProducerCounts producer;

    // consumer side: written by the CS thread only, which also takes the window. Atomics so a
    // stray second writer (a second device) is a lost count, not undefined behaviour; the
    // increment is a relaxed load and store, no read-modify-write.
    std::atomic<uint64_t> runs          = { 0u };
    std::atomic<uint64_t> bytes         = { 0u };
    std::atomic<uint64_t> parks         = { 0u };
    std::atomic<uint64_t> parkTicks     = { 0u };
    std::atomic<uint64_t> syncWakes     = { 0u };
    std::atomic<uint64_t> cmds[kKinds]  = { };
    // the current window's start (cheap-clock ticks); 0: not started
    std::atomic<uint64_t> windowStart   = { 0u };

    static void add(std::atomic<uint64_t>& a, uint64_t n) {
      a.store(a.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }

    void startOnce(uint64_t now) {
      uint64_t zero = 0u;
      windowStart.compare_exchange_strong(zero, now, std::memory_order_relaxed);
    }

    // producer events

    void onHandover(bool consumerParked) {
      producerLock.lock();
      producer.chunks++;
      producer.wakes += consumerParked ? 1u : 0u;
      producerLock.unlock();
    }

    void onSyncWait(uint64_t ticks) {
      producerLock.lock();
      producer.syncWaits++;
      producer.syncWaitTicks += ticks;
      producerLock.unlock();
    }

    // consumer events (the CS thread)

    void onPark(uint64_t ticks) {
      add(parks, 1u);
      add(parkTicks, ticks);
    }

    void onSyncWake() {
      add(syncWakes, 1u);
    }

    void onRun(uint64_t chunkBytes) {
      add(runs, 1u);
      add(bytes, chunkBytes);
    }

    void onCommand(DxvkCsCmdKind kind) {
      add(cmds[uint32_t(kind)], 1u);
    }

    // Ends the window at `now`: its raw totals, then everything restarts from zero.
    Snapshot takeSnapshot(uint64_t now) {
      auto get = [] (std::atomic<uint64_t>& a) { return a.exchange(0u, std::memory_order_relaxed); };
      Snapshot s;

      producerLock.lock();
      s.producer = producer;
      producer = ProducerCounts();
      producerLock.unlock();

      s.runs      = get(runs);
      s.bytes     = get(bytes);
      s.parks     = get(parks);
      s.parkTicks = get(parkTicks);
      s.syncWakes = get(syncWakes);
      for (uint32_t i = 0u; i < kKinds; i++)
        s.cmds[i] = get(cmds[i]);

      const uint64_t start = windowStart.exchange(now, std::memory_order_relaxed);
      s.wallTicks = start ? cheap_clock::elapsed(start, now) : 0u;
      return s;
    }

    // Ends the window at `now`: the PROF-CS line for `frame` over `frames` frames.
    std::string take(uint64_t frame, uint32_t frames, const cheap_clock::Calibration& cal, uint64_t now) {
      return format(takeSnapshot(now), frame, frames, cal);
    }

    static std::string format(const Snapshot& s, uint64_t frame, uint32_t frames,
                              const cheap_clock::Calibration& cal) {
      const double f = frames ? double(frames) : 1.0;
      const double wallMs = cal.toMs(s.wallTicks);
      const double parkMs = cal.toMs(s.parkTicks);
      const double syncMs = cal.toMs(s.producer.syncWaitTicks);
      const double busyMs = wallMs > parkMs ? wallMs - parkMs : 0.0;
      uint64_t total = 0u;
      for (uint32_t i = 0u; i < kKinds; i++)
        total += s.cmds[i];
      auto k = [&] (DxvkCsCmdKind kind) { return double(s.cmds[uint32_t(kind)]) / f; };

      char buf[640];
      std::snprintf(buf, sizeof(buf),
        "PROF-CS frame=%llu win=%u per-frame:"
        " csChunk=%.2f csWake=%.2f csSyncWait=%.3f csSyncWaitMs=%.4f"
        " | csRun=%.2f csKiB=%.1f csPark=%.2f csParkMs=%.3f csBusyMs=%.3f csSyncWake=%.3f"
        " | cmd=%.1f cmdDraw=%.1f cmdVb=%.1f cmdIb=%.1f cmdConst=%.1f cmdTex=%.1f"
        " cmdShader=%.1f cmdState=%.1f cmdProf=%.1f cmdOther=%.1f",
        (unsigned long long) frame, frames,
        double(s.producer.chunks) / f, double(s.producer.wakes) / f,
        double(s.producer.syncWaits) / f, syncMs / f,
        double(s.runs) / f, double(s.bytes) / 1024.0 / f, double(s.parks) / f, parkMs / f,
        busyMs / f, double(s.syncWakes) / f,
        double(total) / f,
        k(DxvkCsCmdKind::Draw), k(DxvkCsCmdKind::VertexBuffer), k(DxvkCsCmdKind::IndexBuffer),
        k(DxvkCsCmdKind::Constants), k(DxvkCsCmdKind::Texture), k(DxvkCsCmdKind::Shader),
        k(DxvkCsCmdKind::State), k(DxvkCsCmdKind::Prof), k(DxvkCsCmdKind::Other));
      return buf;
    }
  };

  inline Stats g_stats;

  // The profiling-only loop of DxvkCsChunk::executeAll: the same order, and for a single-use chunk
  // the same destruction, as the original loop, plus the counts. The chunk counts (csRun, csKiB)
  // before its first command; each command counts right before it runs, so a command that ends the
  // window (EndFrame -> PROF) is in it and the commands after it in the same chunk are in the next.
  // Cmd: next(), kind(), exec(ctx) and a virtual destructor (DxvkCsCmd).
  template<typename Cmd, typename Ctx>
  void runCounted(Stats& s, Cmd* cmd, Ctx* ctx, uint64_t chunkBytes, bool singleUse) {
    s.onRun(chunkBytes);

    while (cmd != nullptr) {
      Cmd* next = cmd->next();
      s.onCommand(cmd->kind());
      cmd->exec(ctx);
      if (singleUse)
        cmd->~Cmd();
      cmd = next;
    }
  }

  // The profiling-only sync wait (DxvkCsThread::synchronize, injectChunk with synchronize). Called
  // with the counter mutex held. A chunk already done (`done()` true under the mutex) returns at
  // once and counts nothing; otherwise `waiters` (guarded by that mutex, read by the CS thread for
  // csSyncWake) covers the wait, and its time counts as one csSyncWait.
  template<typename Clock, typename Done, typename Wait>
  void timedWait(Stats& s, uint32_t& waiters, Done&& done, Wait&& wait) {
    if (done())
      return;
    const uint64_t t0 = Clock::now();
    waiters++;
    wait();
    waiters--;
    s.onSyncWait(cheap_clock::elapsed(t0, Clock::now()));
  }

  // The production clock for timedWait and the park timer; a test substitutes its own.
  struct CheapClock {
    static uint64_t now() { return cheap_clock::now(); }
  };

}
