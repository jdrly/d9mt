#pragma once

// D9MT_PROFILE: the CS handoff between the game thread and the CS thread (d9mt #127, lane OPT's
// R1b). One PROF-CS line follows every PROF line (every 256 frames, on the CS thread):
//
//   PROF-CS frame=512 win=256 per-frame: csChunk=61.2 csWake=40.1 csSyncWait=0.02
//     csSyncWaitMs=0.001 | csRun=61.2 csKiB=930.5 csPark=40.1 csParkMs=12.40 csBusyMs=4.27
//     csSyncWake=0.02 | cmd=21890.1 cmdDraw=5410.0 cmdVb=7811.2 cmdIb=1203.4 cmdConst=5590.3
//     cmdTex=812.0 cmdShader=301.2 cmdState=640.0 cmdProf=118.0 cmdOther=4.0
//
// Every value is per frame (the window's total / 256). One line, keys unique on it, all new (none
// is a key of the PROF line either), so parsers that read `key=value` pairs can't mix them up.
//
// Producer side (the threads that hand chunks to the CS thread; for SWTOR the game thread):
//   csChunk       chunks handed over (DxvkCsThread::dispatchChunk and injectChunk)
//   csWake        handovers that found the CS thread parked on its queue's condition variable:
//                 the notify had a sleeper to wake (under Wine, the thread-alert path). Counted
//                 once per park, however many chunks arrive before the CS thread runs again.
//   csSyncWait    waits for the CS thread to reach a chunk (synchronize(), injectChunk(..., true))
//   csSyncWaitMs  their time, ms per frame
// Consumer side (the CS thread):
//   csRun         chunks executed
//   csKiB         chunk bytes executed (commands and their data), KiB per frame
//   csPark        times the CS thread found its queues empty and slept on the condition variable
//   csParkMs      time parked, ms per frame
//   csBusyMs      the window's wall time minus csParkMs, ms per frame: everything the CS thread
//                 did while not parked, including waits inside commands (syncWsi, GPU waits;
//                 PROF's buckets break part of it down). Exact for one device: the CS thread
//                 writes the line itself, so it is never parked at a window's end. With several
//                 devices the park time is summed over their CS threads (clamped at 0).
//   csSyncWake    completed chunks whose notify found a thread waiting in csSyncWait
// Commands executed, by kind (DxvkCsCmdKind: a template argument at the emit site, EmitCsAs):
//   cmd           all commands
//   cmdDraw       draw commands; consecutive draws merged into one command count once (PROF's
//                 draws= counts each draw)
//   cmdVb cmdIb   vertex and index buffer binds
//   cmdConst      constant uploads and binds (constant ring ranges, push data, spec constants)
//   cmdTex        texture and sampler binds
//   cmdShader     shader, input layout and FF/linkage binds
//   cmdState      render state: framebuffer, viewport/scissor, blend, depth/stencil, raster,
//                 multisample, primitive type
//   cmdProf       ProfApi's API tags: commands D9MT_PROFILE itself adds (subtract for the
//                 profile-off count)
//   cmdOther      everything untagged: clears, copies, uploads, flushes, queries, frame markers
//
// Off (D9MT_PROFILE unset): the kind is a template argument of the command class
// (DxvkCsTypedCmd<T, K>), so a tagged emit builds and moves the lambda exactly as an untagged
// one, and chunks pack the same bytes. Every other addition is one not-taken branch on g_enabled
// per chunk handover, per chunk executed, per park and per sync wait, never per command.
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

  // Counters of one window. Producer fields are written by any thread that hands over chunks,
  // consumer fields by the CS thread(s); all relaxed atomics, read and reset by the CS thread
  // that writes the line. Only touched with g_enabled.
  struct Stats {
    // producer side
    std::atomic<uint64_t> chunks        = { 0u };
    std::atomic<uint64_t> wakes         = { 0u };
    std::atomic<uint64_t> syncWaits     = { 0u };
    std::atomic<uint64_t> syncWaitTicks = { 0u };
    // consumer side
    std::atomic<uint64_t> runs          = { 0u };
    std::atomic<uint64_t> bytes         = { 0u };
    std::atomic<uint64_t> parks         = { 0u };
    std::atomic<uint64_t> parkTicks     = { 0u };
    std::atomic<uint64_t> syncWakes     = { 0u };
    std::atomic<uint64_t> cmds[kKinds]  = { };
    // the current window's start (cheap-clock ticks); 0: not started
    std::atomic<uint64_t> windowStart   = { 0u };

    void startOnce(uint64_t now) {
      uint64_t zero = 0u;
      windowStart.compare_exchange_strong(zero, now, std::memory_order_relaxed);
    }

    void onHandover(bool consumerParked) {
      chunks.fetch_add(1u, std::memory_order_relaxed);
      if (consumerParked)
        wakes.fetch_add(1u, std::memory_order_relaxed);
    }

    void onSyncWait(uint64_t ticks) {
      syncWaits.fetch_add(1u, std::memory_order_relaxed);
      syncWaitTicks.fetch_add(ticks, std::memory_order_relaxed);
    }

    void onPark(uint64_t ticks) {
      parks.fetch_add(1u, std::memory_order_relaxed);
      parkTicks.fetch_add(ticks, std::memory_order_relaxed);
    }

    void onSyncWake() {
      syncWakes.fetch_add(1u, std::memory_order_relaxed);
    }

    // One executed chunk: its bytes and its commands by kind (counted by the caller).
    void onRun(uint64_t chunkBytes, const uint32_t (&kinds)[kKinds]) {
      runs.fetch_add(1u, std::memory_order_relaxed);
      bytes.fetch_add(chunkBytes, std::memory_order_relaxed);
      for (uint32_t i = 0u; i < kKinds; i++) {
        if (kinds[i])
          cmds[i].fetch_add(kinds[i], std::memory_order_relaxed);
      }
    }

    // Ends the window at `now`: the PROF-CS line for `frame` over `frames` frames, then reset.
    std::string take(uint64_t frame, uint32_t frames, const cheap_clock::Calibration& cal, uint64_t now) {
      auto get = [] (std::atomic<uint64_t>& a) { return a.exchange(0u, std::memory_order_relaxed); };
      const double f = frames ? double(frames) : 1.0;

      const uint64_t start = windowStart.exchange(now, std::memory_order_relaxed);
      const double wallMs = start ? cal.toMs(cheap_clock::elapsed(start, now)) : 0.0;

      const uint64_t nChunks   = get(chunks);
      const uint64_t nWakes    = get(wakes);
      const uint64_t nSync     = get(syncWaits);
      const double   syncMs    = cal.toMs(get(syncWaitTicks));
      const uint64_t nRuns     = get(runs);
      const uint64_t nBytes    = get(bytes);
      const uint64_t nParks    = get(parks);
      const double   parkMs    = cal.toMs(get(parkTicks));
      const uint64_t nSyncWake = get(syncWakes);
      uint64_t k[kKinds];
      uint64_t total = 0u;
      for (uint32_t i = 0u; i < kKinds; i++)
        total += (k[i] = get(cmds[i]));
      const double busyMs = wallMs > parkMs ? wallMs - parkMs : 0.0;

      char buf[640];
      std::snprintf(buf, sizeof(buf),
        "PROF-CS frame=%llu win=%u per-frame:"
        " csChunk=%.2f csWake=%.2f csSyncWait=%.3f csSyncWaitMs=%.4f"
        " | csRun=%.2f csKiB=%.1f csPark=%.2f csParkMs=%.3f csBusyMs=%.3f csSyncWake=%.3f"
        " | cmd=%.1f cmdDraw=%.1f cmdVb=%.1f cmdIb=%.1f cmdConst=%.1f cmdTex=%.1f"
        " cmdShader=%.1f cmdState=%.1f cmdProf=%.1f cmdOther=%.1f",
        (unsigned long long) frame, frames,
        double(nChunks) / f, double(nWakes) / f, double(nSync) / f, syncMs / f,
        double(nRuns) / f, double(nBytes) / 1024.0 / f, double(nParks) / f, parkMs / f,
        busyMs / f, double(nSyncWake) / f,
        double(total) / f,
        double(k[uint32_t(DxvkCsCmdKind::Draw)]) / f,
        double(k[uint32_t(DxvkCsCmdKind::VertexBuffer)]) / f,
        double(k[uint32_t(DxvkCsCmdKind::IndexBuffer)]) / f,
        double(k[uint32_t(DxvkCsCmdKind::Constants)]) / f,
        double(k[uint32_t(DxvkCsCmdKind::Texture)]) / f,
        double(k[uint32_t(DxvkCsCmdKind::Shader)]) / f,
        double(k[uint32_t(DxvkCsCmdKind::State)]) / f,
        double(k[uint32_t(DxvkCsCmdKind::Prof)]) / f,
        double(k[uint32_t(DxvkCsCmdKind::Other)]) / f);
      return buf;
    }
  };

  inline Stats g_stats;

}
