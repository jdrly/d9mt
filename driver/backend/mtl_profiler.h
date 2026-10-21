#pragma once

// Buckets nest and are not mutually exclusive — interpret a sub-bucket as a
// breakdown of its parent, not as additive across the whole frame.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk::backend::prof {

  enum Bucket : uint32_t {
    BeginRecording = 0,
    PrepareDraw,
    ResolvePso,
    FlushVertexBuffers,
    FlushResources,
    FlushDynamicState,
    EncodeDraw,
    FlushCmdChunk,
    Submit,
    SynchronizeWsi,
    EndFrame,
    BucketCount
  };

  enum Counter : uint32_t {
    CntDraws = 0,
    CntChunkFlush,
    CntPsoMiss,
    CntArgRotate,
    CntAbRebuild,
    CntPsoKeyBuild,
    CntPassCloseSubmit,
    CntPassCloseRt,
    CntPassCloseClear,
    CntPassCloseXfer,
    CntPassCloseBarrier,
    CntPassCloseBlit,
    CntFramePasses,
    CntStoreDontCare,
    CntVtxFence,
    CounterCount
  };

  inline const bool g_profEnabled = std::getenv("D9MT_PROFILE") != nullptr;

  inline uint64_t g_ns[BucketCount]     = {};
  inline uint64_t g_counts[CounterCount] = {};
  inline uint64_t g_frames = 0u;

  // MTLSharedEvent_signaledValue reads (SubmissionTracker::completedSerial): any thread, so
  // atomic, and outside g_counts.
  inline std::atomic<uint64_t> g_eventReads = { 0u };

  inline void countEventRead() {
    if (!g_profEnabled) return;
    g_eventReads.fetch_add(1u, std::memory_order_relaxed);
  }

  // SubmissionTracker::waitFor() calls that started on a serial not submitted yet, and how
  // long they waited (G1's hazard: a carry holding back a submit someone waits on; also the
  // older WaitForResource race, which ends when the CS thread submits). Any thread.
  inline std::atomic<uint64_t> g_unsubWaits     = { 0u };
  inline std::atomic<uint64_t> g_unsubWaitNs    = { 0u };
  inline std::atomic<uint64_t> g_unsubWaitMaxNs = { 0u };

  inline void noteUnsubmittedWait(uint64_t ns, uint64_t serial) {
    // One line per event too: short runs (the hash suite's 5 frames, ExitProcess) never reach
    // a PROF window. Rare by nature; only with D9MT_PROFILE.
    Logger::info(str::format("PROF-UNSUB serial=", serial, " waitedUs=", ns / 1000u));
    g_unsubWaits.fetch_add(1u, std::memory_order_relaxed);
    g_unsubWaitNs.fetch_add(ns, std::memory_order_relaxed);
    uint64_t max = g_unsubWaitMaxNs.load(std::memory_order_relaxed);
    while (ns > max && !g_unsubWaitMaxNs.compare_exchange_weak(max, ns, std::memory_order_relaxed)) { }
  }

  // The drawable acquirer (MtlPresenterImpl), unlike the presenter's log lines (20 per 10 s) not
  // rate-limited, so the PROF line can compare builds: nextDrawable calls and their time on the
  // acquirer thread, the CS thread's waits for a drawable in takeDrawable, and the presents
  // skipped because none came in time. Any thread.
  inline std::atomic<uint64_t> g_acquires         = { 0u };
  inline std::atomic<uint64_t> g_acquireNs        = { 0u };
  inline std::atomic<uint64_t> g_acquireMaxNs     = { 0u };
  inline std::atomic<uint64_t> g_drawableWaits    = { 0u };
  inline std::atomic<uint64_t> g_drawableWaitNs   = { 0u };
  inline std::atomic<uint64_t> g_drawableWaitMaxNs = { 0u };
  inline std::atomic<uint64_t> g_drawableMisses   = { 0u };

  inline void atomicMax(std::atomic<uint64_t>& m, uint64_t v) {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) { }
  }

  inline void noteAcquire(uint64_t ns) {
    if (!g_profEnabled) return;
    g_acquires.fetch_add(1u, std::memory_order_relaxed);
    g_acquireNs.fetch_add(ns, std::memory_order_relaxed);
    atomicMax(g_acquireMaxNs, ns);
  }

  inline void noteDrawableWait(uint64_t ns, bool got) {
    if (!g_profEnabled) return;
    g_drawableWaits.fetch_add(1u, std::memory_order_relaxed);
    g_drawableWaitNs.fetch_add(ns, std::memory_order_relaxed);
    atomicMax(g_drawableWaitMaxNs, ns);
    if (!got)
      g_drawableMisses.fetch_add(1u, std::memory_order_relaxed);
  }

  struct Scope {
    std::chrono::steady_clock::time_point t0;
    uint32_t bucket;
    explicit Scope(Bucket b) : bucket(b) {
      if (!g_profEnabled) return;
      t0 = std::chrono::steady_clock::now();
    }
    ~Scope() {
      if (!g_profEnabled) return;
      g_ns[bucket] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - t0).count());
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  };

  inline void bump(Counter c, uint64_t n = 1u) {
    if (!g_profEnabled) return;
    g_counts[c] += n;
  }

  inline void onFrameEnd() {
    if (!g_profEnabled) return;
    if ((++g_frames & 0xFFu) != 0u)
      return;

    static constexpr const char* kNames[BucketCount] = {
      "beginRecording", "prepareDraw", "resolvePso", "flushVB",
      "flushRes", "flushDyn", "encodeDraw", "flushChunk",
      "submit", "syncWsi", "endFrame"
    };
    constexpr double kFrames = 256.0;

    std::string line = str::format("PROF frame=", g_frames, " win=256 ms(avg/f):");
    for (uint32_t i = 0u; i < BucketCount; i++) {
      double totalMs = double(g_ns[i]) * 1e-6;
      line += str::format(" ", kNames[i], "=",
        totalMs, "(", totalMs / kFrames, ")");
      g_ns[i] = 0u;
    }
    line += str::format(" | draws=", g_counts[CntDraws],
      "(", double(g_counts[CntDraws]) / kFrames, "/f)",
      " chunkFlush=", g_counts[CntChunkFlush],
      "(", double(g_counts[CntChunkFlush]) / kFrames, "/f)",
      " psoMiss=", g_counts[CntPsoMiss],
      " argRotate=", g_counts[CntArgRotate],
      "(", double(g_counts[CntArgRotate]) / kFrames, "/f)",
      " abRebuild=", g_counts[CntAbRebuild],
      "(", double(g_counts[CntAbRebuild]) / kFrames, "/f)",
      " psoKeyBuild=", g_counts[CntPsoKeyBuild],
      "(", double(g_counts[CntPsoKeyBuild]) / kFrames, "/f)",
      " passClose/f: submit=", double(g_counts[CntPassCloseSubmit]) / kFrames,
      " rt=", double(g_counts[CntPassCloseRt]) / kFrames,
      " clear=", double(g_counts[CntPassCloseClear]) / kFrames,
      " xfer=", double(g_counts[CntPassCloseXfer]) / kFrames,
      " barrier=", double(g_counts[CntPassCloseBarrier]) / kFrames,
      " blit=", double(g_counts[CntPassCloseBlit]) / kFrames,
      " | framePasses=", double(g_counts[CntFramePasses]) / kFrames, "/f",
      " storeDontCare=", double(g_counts[CntStoreDontCare]) / kFrames, "/f",
      " vtxFence=", double(g_counts[CntVtxFence]) / kFrames, "/f");
    const uint64_t eventReads = g_eventReads.exchange(0u, std::memory_order_relaxed);
    line += str::format(" evRead=", eventReads, "(", double(eventReads) / kFrames, "/f)");
    const uint64_t unsubWaits = g_unsubWaits.exchange(0u, std::memory_order_relaxed);
    const uint64_t unsubNs    = g_unsubWaitNs.exchange(0u, std::memory_order_relaxed);
    const uint64_t unsubMaxNs = g_unsubWaitMaxNs.exchange(0u, std::memory_order_relaxed);
    line += str::format(" unsubWait=", unsubWaits, "(", double(unsubWaits) / kFrames, "/f)",
      " unsubWaitUs=", unsubNs / 1000u, "(max ", unsubMaxNs / 1000u, ")");
    const uint64_t acq    = g_acquires.exchange(0u, std::memory_order_relaxed);
    const uint64_t acqNs  = g_acquireNs.exchange(0u, std::memory_order_relaxed);
    const uint64_t acqMax = g_acquireMaxNs.exchange(0u, std::memory_order_relaxed);
    const uint64_t dw     = g_drawableWaits.exchange(0u, std::memory_order_relaxed);
    const uint64_t dwNs   = g_drawableWaitNs.exchange(0u, std::memory_order_relaxed);
    const uint64_t dwMax  = g_drawableWaitMaxNs.exchange(0u, std::memory_order_relaxed);
    const uint64_t miss   = g_drawableMisses.exchange(0u, std::memory_order_relaxed);
    line += str::format(" | nextDrawable=", acq, " ms(avg/max)=", acq ? double(acqNs) * 1e-6 / double(acq) : 0.0,
      "/", double(acqMax) * 1e-6,
      " drawableWait=", dw, " ms(avg/max)=", dw ? double(dwNs) * 1e-6 / double(dw) : 0.0, "/", double(dwMax) * 1e-6,
      " noDrawable=", miss);
    for (uint32_t i = 0u; i < CounterCount; i++)
      g_counts[i] = 0u;

    Logger::info(line);
  }

}
