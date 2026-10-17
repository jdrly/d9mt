#pragma once

// D9MT_PROFILE hitch attribution: per Present, the app-thread time spent inside
// d9mt (outermost device-locked calls plus the creation entry points that
// don't lock), broken down by category. A frame longer than D9MT_HITCH_MS
// (default 50) logs one warn-level HITCH line, so a long frame can be told
// apart as driver work (inD9mtMs close to dtMs) or the game's own (disk
// streaming, its own threads: inD9mtMs small).
//
// Off (D9MT_PROFILE unset): one predictable branch per scope. On: rdtsc (on
// arm64 and ARM64EC the virtual counter) and a few relaxed atomics per scope; ticks
// are converted with a ratio calibrated against the steady clock at every frame.
//
// Header-only and dependency-free so the formatting and the frame logic can
// be unit-tested natively (test/native/hitch_test.cpp).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#if (defined(__x86_64__) && !defined(__arm64ec__)) || defined(__i386__) // ARM64EC defines __x86_64__
#include <x86intrin.h>
#endif

namespace dxvk::hitch {

  inline const bool g_enabled = std::getenv("D9MT_PROFILE") != nullptr;

  // Categories nest (bufLock contains its csSync/gpuWait, present contains
  // latencyWait): read a nested one as a breakdown, not as additive.
  enum Cat : uint32_t {
    Present = 0,  // whole Present call (incl. frame latency wait, background throttle sleep)
    Latency,      // waiting for the CS thread / GPU to present frame N - maxLatency
    TexCreate,    // CreateTexture/VolumeTexture/CubeTexture + surfaces; bytes = mip chain size
    TexLock,      // texture LockRect/LockBox + UnlockRect/UnlockBox (incl. upload on unlock)
    TexUpload,    // staging copy + GPU upload (unlock, UpdateTexture/Surface, managed at draw); bytes
    BufCreate,    // CreateVertexBuffer/IndexBuffer; bytes
    BufLock,      // buffer Lock + Unlock; aux = locks that had to call WaitForResource
    ShaderCreate, // CreateVertexShader/PixelShader
    CsSync,       // app thread waiting for the CS thread to catch up
    GpuWait,      // app thread waiting for a resource on the GPU
    Throttle,     // staging memory throttle waiting for the GPU
    Query,        // IDirect3DQuery9::GetData
    Count
  };

  inline const char* catName(uint32_t c) {
    static constexpr const char* kNames[Count] = {
      "present", "latencyWait", "texCreate", "texLock", "texUpload", "bufCreate",
      "bufLock", "shaderCreate", "csSync", "gpuWait", "throttle", "query"
    };
    return c < Count ? kNames[c] : "?";
  }

  inline uint64_t ticks() {
#if (defined(__x86_64__) && !defined(__arm64ec__)) || defined(__i386__)
    return __rdtsc();
#else // arm64, ARM64EC: the virtual counter, like rdtsc not ordered against other code
    uint64_t v; __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#endif
  }

  inline uint64_t nowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  // D9MT_HITCH_MS, default 50.
  inline double thresholdFromEnv() {
    const char* v = std::getenv("D9MT_HITCH_MS");
    double ms = v ? std::strtod(v, nullptr) : 0.0;
    return ms > 0.0 ? ms : 50.0;
  }

  // One frame's numbers, already converted to ms.
  struct FrameStats {
    struct Entry {
      uint64_t n     = 0u;
      double   ms    = 0.0;
      uint64_t bytes = 0u;
      uint64_t aux   = 0u;
    };

    double   dtMs     = 0.0;
    double   inMs     = 0.0;
    uint64_t csBehind = 0u;  // CS chunks dispatched but not executed at frame end
    Entry    cat[Count];

    // CS thread / compile workers, from the COMPILE counters (not app-thread time).
    uint64_t shaders = 0u, psos = 0u, renderWaits = 0u;
    double   shadersMs = 0.0, psosMs = 0.0, renderWaitMs = 0.0;
    // Draws dropped while their pipeline compiled (D9MT_ASYNC_SKIP or the wait
    // budget) and waits the budget cut short; only shown when non-zero.
    uint64_t skipped = 0u, budgetHits = 0u;
  };

  inline std::string formatHitch(uint64_t frame, const FrameStats& s, uint32_t maxCats = 8u) {
    constexpr double MiB = 1.0 / double(1u << 20);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
      "HITCH frame=%llu dtMs=%.1f inD9mtMs=%.1f (%.0f%%) texMiB created=%.1f uploaded=%.1f csBehind=%llu |",
      (unsigned long long)frame, s.dtMs, s.inMs, s.dtMs > 0.0 ? 100.0 * s.inMs / s.dtMs : 0.0,
      double(s.cat[TexCreate].bytes) * MiB, double(s.cat[TexUpload].bytes) * MiB,
      (unsigned long long)s.csBehind);
    std::string line = buf;

    uint32_t order[Count];
    uint32_t used = 0u;
    for (uint32_t i = 0u; i < Count; i++) {
      if (s.cat[i].n)
        order[used++] = i;
    }
    std::stable_sort(order, order + used, [&s] (uint32_t a, uint32_t b) {
      return s.cat[a].ms > s.cat[b].ms;
    });

    if (!used)
      line += " none";

    for (uint32_t k = 0u; k < used && k < maxCats; k++) {
      const FrameStats::Entry& e = s.cat[order[k]];
      std::snprintf(buf, sizeof(buf), " %s=%llu/%.1fms", catName(order[k]),
        (unsigned long long)e.n, e.ms);
      line += buf;
      if (e.bytes) {
        std::snprintf(buf, sizeof(buf), "/%.1fMiB", double(e.bytes) * MiB);
        line += buf;
      }
      if (order[k] == BufLock) {
        std::snprintf(buf, sizeof(buf), "(waits %llu)", (unsigned long long)e.aux);
        line += buf;
      }
    }

    std::snprintf(buf, sizeof(buf), " | cs: renderWait=%llu/%.1fms shaders=%llu/%.1fms psos=%llu/%.1fms",
      (unsigned long long)s.renderWaits, s.renderWaitMs,
      (unsigned long long)s.shaders, s.shadersMs,
      (unsigned long long)s.psos, s.psosMs);
    line += buf;
    if (s.skipped || s.budgetHits) {
      std::snprintf(buf, sizeof(buf), " skipped=%llu budgetHit=%llu",
        (unsigned long long)s.skipped, (unsigned long long)s.budgetHits);
      line += buf;
    }
    return line;
  }


  class Profiler {

  public:

    struct CompileTotals {
      uint64_t shaders = 0u, shadersNs = 0u;
      uint64_t psos = 0u, psosNs = 0u;
      uint64_t waits = 0u, waitsNs = 0u;
      uint64_t skipped = 0u, budgetHits = 0u;
    };

    // Outermost d9mt entry on this device (depth 0 -> 1) counts towards inD9mt.
    bool enter() {
      return m_depth.fetch_add(1, std::memory_order_relaxed) == 0;
    }

    void leave(bool outer, uint64_t t0, uint64_t t1) {
      if (outer)
        m_inside.fetch_add(t1 - t0, std::memory_order_relaxed);
      m_depth.fetch_sub(1, std::memory_order_relaxed);
    }

    void add(Cat c, uint64_t count, uint64_t dticks, uint64_t bytes, uint64_t aux) {
      Bucket& b = m_cat[c];
      if (count)
        b.n.fetch_add(count, std::memory_order_relaxed);
      b.ticks.fetch_add(dticks, std::memory_order_relaxed);
      if (bytes)
        b.bytes.fetch_add(bytes, std::memory_order_relaxed);
      if (aux)
        b.aux.fetch_add(aux, std::memory_order_relaxed);
    }

    // Called once per Present, after it returned. Fills `out` and returns true
    // if the frame (Present to Present) took at least the threshold.
    bool endFrame(uint64_t nowTicks, uint64_t nowNanos, const CompileTotals& compile,
                  uint64_t csBehind, double thresholdMs, uint64_t& frameOut, FrameStats& out) {
      if (!m_calibTicks) {
        m_calibTicks = nowTicks;
        m_calibNs    = nowNanos;
      }

      const uint64_t frame = ++m_frame;
      const double dtMs = m_lastNs ? double(nowNanos - m_lastNs) * 1e-6 : 0.0;
      m_lastNs = nowNanos;

      // ticks -> ns, from everything seen so far (needs some elapsed time).
      const uint64_t calNs = nowNanos - m_calibNs;
      if (calNs >= 20000000u && nowTicks > m_calibTicks)
        m_nsPerTick = double(calNs) / double(nowTicks - m_calibTicks);

      FrameStats s;
      s.dtMs     = dtMs;
      s.inMs     = double(m_inside.exchange(0u, std::memory_order_relaxed)) * m_nsPerTick * 1e-6;
      s.csBehind = csBehind;
      for (uint32_t i = 0u; i < Count; i++) {
        Bucket& b = m_cat[i];
        s.cat[i].n     = b.n.exchange(0u, std::memory_order_relaxed);
        s.cat[i].ms    = double(b.ticks.exchange(0u, std::memory_order_relaxed)) * m_nsPerTick * 1e-6;
        s.cat[i].bytes = b.bytes.exchange(0u, std::memory_order_relaxed);
        s.cat[i].aux   = b.aux.exchange(0u, std::memory_order_relaxed);
      }

      s.shaders      = compile.shaders - m_compile.shaders;
      s.shadersMs    = double(compile.shadersNs - m_compile.shadersNs) * 1e-6;
      s.psos         = compile.psos - m_compile.psos;
      s.psosMs       = double(compile.psosNs - m_compile.psosNs) * 1e-6;
      s.renderWaits  = compile.waits - m_compile.waits;
      s.renderWaitMs = double(compile.waitsNs - m_compile.waitsNs) * 1e-6;
      s.skipped      = compile.skipped - m_compile.skipped;
      s.budgetHits   = compile.budgetHits - m_compile.budgetHits;
      m_compile = compile;

      // The first frame has no previous Present; before calibration the
      // tick ratio is a guess.
      if (frame < 2u || m_nsPerTick <= 0.0 || dtMs < thresholdMs)
        return false;

      frameOut = frame;
      out = s;
      return true;
    }

  private:

    struct Bucket {
      std::atomic<uint64_t> n     = { 0u };
      std::atomic<uint64_t> ticks = { 0u };
      std::atomic<uint64_t> bytes = { 0u };
      std::atomic<uint64_t> aux   = { 0u };
    };

    std::atomic<int32_t>  m_depth  = { 0 };
    std::atomic<uint64_t> m_inside = { 0u };
    Bucket                m_cat[Count];

    // Frame state: Present's thread only.
    uint64_t      m_frame      = 0u;
    uint64_t      m_lastNs     = 0u;
    uint64_t      m_calibTicks = 0u;
    uint64_t      m_calibNs    = 0u;
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) // counters, calibrated
    double        m_nsPerTick  = 0.0;
#else
    double        m_nsPerTick  = 1.0;
#endif
    CompileTotals m_compile;

  };


  // Times a category. `entry`: an API entry point that doesn't take the device
  // lock, so it counts towards inD9mt itself (nested scopes don't).
  class Scope {

  public:

    Scope(Profiler& profiler, Cat c, bool entry = false) {
      if (__builtin_expect(!g_enabled, 1))
        return;
      m_p     = &profiler;
      m_c     = c;
      m_entry = entry;
      m_outer = entry && profiler.enter();
      m_t0    = ticks();
    }

    ~Scope() {
      if (__builtin_expect(m_p == nullptr, 1))
        return;
      const uint64_t t1 = ticks();
      m_p->add(m_c, m_count, t1 - m_t0, m_bytes, m_aux);
      if (m_entry)
        m_p->leave(m_outer, m_t0, t1);
    }

    void addBytes(uint64_t bytes) { m_bytes += bytes; }
    void addAux(uint64_t n = 1u)  { m_aux += n; }
    // Time only (e.g. the Unlock of a Lock/Unlock pair counted once).
    void noCount()                { m_count = 0u; }

    Scope(const Scope&) = delete;
    Scope& operator = (const Scope&) = delete;

  private:

    Profiler* m_p     = nullptr;
    uint64_t  m_t0    = 0u;
    uint64_t  m_bytes = 0u;
    uint64_t  m_aux   = 0u;
    uint32_t  m_count = 1u;
    Cat       m_c     = Present;
    bool      m_entry = false;
    bool      m_outer = false;

  };

}
