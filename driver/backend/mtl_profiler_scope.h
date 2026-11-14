#pragma once

// PROF's timed buckets and counters (D9MT_PROFILE): the scope, the per-bucket tick totals, the
// counters and the 256-frame window. Buckets nest and are not mutually exclusive: read a
// sub-bucket as a breakdown of its parent, not as additive across the whole frame.
//
// Scopes accumulate raw ticks of the shared cheap clock (util_cheap_clock.h, d9mt #126); the
// window converts them to ms once, when it rolls over. The CS thread is the only writer of the
// buckets and the counters, and the only reader (onFrameEnd runs in one of its commands).
//
// Dependency-free so the scope, counter and window logic is unit-tested natively with a
// deterministic clock (test/native/prof_clock_test.cpp). Formatting and logging stay in
// mtl_profiler.h.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../util/util_cheap_clock.h"

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
    CsExec,     // the CS thread executing chunks (d9mt #127); the chunk that rolls the window
                // over is counted in the next one
    AllocWait,  // beginRecording blocked on a command allocator: the in-flight cap is exhausted
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
    CntCsChunk,     // ordered CS chunks executed (the front end's EmitCsChunk)
    CntCsInject,    // high-priority CS chunks executed (InjectCsChunk)
    CntCsCmd,       // CS commands in those chunks
    CntCsBytes,     // bytes those chunks used (commands and their data)
    CntCsSleep,     // the CS thread found its queues empty and waited: each needed a wake
    CntAllocWait,   // beginRecording waits for a command allocator (the AllocWait bucket's count)
    // flushResources per stage (vertex, fragment; d9mt #127): an argument-buffer shader's flush by
    // what made it rebuild the buffer (AbCause), or that it kept the buffer; a table shader's
    // flush, and the table slots it went through (the shadow then skips the unchanged ones)
    CntAbLayoutVs, CntAbLayoutPs,
    CntAbResVs,    CntAbResPs,
    CntAbCbvVs,    CntAbCbvPs,
    CntAbKeepVs,   CntAbKeepPs,
    CntTableVs,    CntTablePs,
    CntTableSlotsVs, CntTableSlotsPs,
    CounterCount
  };

  constexpr uint32_t kWindowFrames = 256u;

  inline const bool g_profEnabled = std::getenv("D9MT_PROFILE") != nullptr;

  // Raw cheap-clock ticks per bucket in the current window.
  inline uint64_t g_ticks[BucketCount] = {};
  inline uint64_t g_frames = 0u;

  // Counts in the current window.
  inline uint64_t g_counts[CounterCount] = {};

  inline void bump(Counter c, uint64_t n = 1u) {
    if (!g_profEnabled) return;
    g_counts[c] += n;
  }

  // One CS chunk the CS thread is about to execute.
  inline void countCsChunk(bool injected, uint64_t commands, uint64_t bytes) {
    if (!g_profEnabled) return;
    g_counts[injected ? CntCsInject : CntCsChunk] += 1u;
    g_counts[CntCsCmd]   += commands;
    g_counts[CntCsBytes] += bytes;
  }

  // Why flushResources rebuilt a stage's argument buffer, first match wins: Layout, the shader
  // (its buffer layout) changed or the stage has no buffer yet; Resources, a texture or sampler
  // binding changed; Cbv, only constant buffer bindings changed (a new range of the constant
  // ring); Keep, nothing changed and the buffer stays.
  enum class AbCause : uint32_t { Layout = 0, Resources, Cbv, Keep };

  inline AbCause abCause(bool layoutChanged, bool noBuffer, uint32_t dirtyCbvs, uint32_t dirtyTextures,
                         uint32_t dirtySamplers) {
    if (layoutChanged || noBuffer)
      return AbCause::Layout;
    if (dirtyTextures | dirtySamplers)
      return AbCause::Resources;
    return dirtyCbvs ? AbCause::Cbv : AbCause::Keep;
  }

  // stage: 0 vertex, 1 fragment.
  inline void countAbFlush(uint32_t stage, AbCause cause) {
    if (!g_profEnabled) return;
    g_counts[CntAbLayoutVs + 2u * uint32_t(cause) + stage] += 1u;
  }

  inline void countTableFlush(uint32_t stage, uint32_t slots) {
    if (!g_profEnabled) return;
    g_counts[CntTableVs + stage]      += 1u;
    g_counts[CntTableSlotsVs + stage] += slots;
  }

  // The window's counts; resets them. onFrameEnd calls it when takeWindow rolls over.
  inline void takeCounts(uint64_t (&out)[CounterCount]) {
    for (uint32_t i = 0u; i < CounterCount; i++) {
      out[i] = g_counts[i];
      g_counts[i] = 0u;
    }
  }

  // The end of the PROF line (d9mt #127), from a rolled-over window: the CS thread's execution
  // time, the chunks, commands and bytes it executed, how often it waited for work, and
  // beginRecording's allocator waits. Totals over the window, per frame in parentheses.
  inline std::string formatCsTransport(const double (&totalMs)[BucketCount], const uint64_t (&c)[CounterCount]) {
    const double f = double(kWindowFrames);
    char buf[384];
    std::snprintf(buf, sizeof(buf),
      " | csExec=%.3f(%.3f) csChunks=%llu(%.2f/f) csInject=%llu(%.2f/f) csCmds=%llu(%.1f/f)"
      " csKiB=%.1f(%.1f/f) csSleep=%llu(%.2f/f) | allocWait=%llu(%.2f/f) allocWaitMs=%.3f(%.3f)",
      totalMs[CsExec], totalMs[CsExec] / f,
      (unsigned long long)c[CntCsChunk], double(c[CntCsChunk]) / f,
      (unsigned long long)c[CntCsInject], double(c[CntCsInject]) / f,
      (unsigned long long)c[CntCsCmd], double(c[CntCsCmd]) / f,
      double(c[CntCsBytes]) / 1024.0, double(c[CntCsBytes]) / 1024.0 / f,
      (unsigned long long)c[CntCsSleep], double(c[CntCsSleep]) / f,
      (unsigned long long)c[CntAllocWait], double(c[CntAllocWait]) / f,
      totalMs[AllocWait], totalMs[AllocWait] / f);
    return buf;
  }

  // The end of the PROF line, after formatCsTransport (d9mt #127): per frame and stage, the
  // argument-buffer flushes as layout/resources/cbv/keep and the table flushes as flushes/slots.
  inline std::string formatBindings(const uint64_t (&c)[CounterCount]) {
    const double f = double(kWindowFrames);
    char buf[320];
    std::snprintf(buf, sizeof(buf),
      " | abVs=%.2f/%.2f/%.2f/%.2f abPs=%.2f/%.2f/%.2f/%.2f tableVs=%.2f/%.2f tablePs=%.2f/%.2f",
      double(c[CntAbLayoutVs]) / f, double(c[CntAbResVs]) / f, double(c[CntAbCbvVs]) / f, double(c[CntAbKeepVs]) / f,
      double(c[CntAbLayoutPs]) / f, double(c[CntAbResPs]) / f, double(c[CntAbCbvPs]) / f, double(c[CntAbKeepPs]) / f,
      double(c[CntTableVs]) / f, double(c[CntTableSlotsVs]) / f,
      double(c[CntTablePs]) / f, double(c[CntTableSlotsPs]) / f);
    return buf;
  }

  // The production clock; a test substitutes a deterministic one.
  struct CheapClock {
    static uint64_t now() { return cheap_clock::now(); }
  };

  template<typename Clock>
  struct BasicScope {
    uint64_t t0 = 0u;
    uint32_t bucket;
    explicit BasicScope(Bucket b) : bucket(b) {
      if (!g_profEnabled) return;
      t0 = Clock::now();
    }
    ~BasicScope() {
      if (!g_profEnabled) return;
      g_ticks[bucket] += cheap_clock::elapsed(t0, Clock::now());
    }
    BasicScope(const BasicScope&) = delete;
    BasicScope& operator=(const BasicScope&) = delete;
  };

  using Scope = BasicScope<CheapClock>;

  // Counts a frame. Every kWindowFrames-th frame, converts the window's buckets to total ms with
  // `cal`, resets them and returns true. Only called with g_profEnabled.
  inline bool takeWindow(const cheap_clock::Calibration& cal, double (&totalMs)[BucketCount]) {
    if ((++g_frames % kWindowFrames) != 0u)
      return false;
    for (uint32_t i = 0u; i < BucketCount; i++) {
      totalMs[i] = cal.toMs(g_ticks[i]);
      g_ticks[i] = 0u;
    }
    return true;
  }

}
