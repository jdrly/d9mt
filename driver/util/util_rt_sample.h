#pragma once

// D9MT_RT_SAMPLE=<Hz>: a sampler of the render thread, the thread that created the device. A
// developer switch, off by default; no app sets it.
//
// Why: a game's render thread spends its time in the game's own code, in DLLs the game calls
// (d3dx9, the C runtime) and in d9mt. PACE measures d9mt's part from inside d9mt; nothing measured
// the rest. A native profiler of the Mac can't help: under Rosetta its stacks lose the x86 leaf.
//
// What it does: a thread of its own suspends the render thread at the rate asked, reads its
// instruction pointer (SuspendThread, GetThreadContext, ResumeThread), and maps it to the module
// that holds it. Every 10 s, and when the device is released, it writes one line with the window's
// sample count and one line per module with its share of the samples. Shares are of wall time:
// a wait counts too, under the module the thread waits in (ntdll.dll for most). A thread inside a
// Unix call (Metal, the wineserver) shows the PE caller's address: winemetal.dll for d9mt's Metal
// calls, ntdll.dll or win32u.dll for Wine's system calls.
//
// Safety. On Wine + Rosetta a thread that gets Wine's suspend signal while it handles a CPU fault
// (an access violation, a breakpoint) can die: Wine ends it with "nested exception on signal
// stack" (seen with a thread faulting in a loop while another one suspended it back to back). An
// exception handler on the Windows side sees a fault only after that window, and never sees the
// faults Wine resolves itself (stack growth). So the sampler watches for faults and backs off:
//   - the first second after the device is created it only watches; if the thread raised a CPU
//     fault by then, it never samples;
//   - afterwards the first CPU fault on the thread stops it for good, with a line that names the
//     code;
//   - between the suspend and the resume it only reads the context into a buffer on its stack:
//     no heap, no lock, no module lookup, no log (a suspended thread may hold any of them).
// The risk left is the first fault itself: a fault that comes while a suspend is in flight.
//
// The pure part (the parse, the module table, the lines) is in this header, so it builds natively
// (test/native/rt_sample_test.cpp); the sampler itself is util_rt_sample.cpp (Windows only).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dxvk::rtsample {

  constexpr uint32_t kMaxHz      = 1000u;
  constexpr uint32_t kReportMs   = 10000u;  // a window
  constexpr uint32_t kWatchMs    = 1000u;   // the first second: watch for faults, no samples
  constexpr uint32_t kMaxModules = 64u;     // modules named in the lines; more share one bucket

  /**
   * \brief The rate D9MT_RT_SAMPLE asks for
   *
   * Unset or empty: 0, valid (off). A whole decimal number from 1 to kMaxHz: that. Anything
   * else (0, a sign, a point, a suffix, a space, more than kMaxHz): 0, and *invalid.
   */
  inline uint32_t hzFromEnv(const char* v, bool* invalid) {
    *invalid = false;
    if (v == nullptr || *v == '\0')
      return 0u;
    uint32_t hz = 0u;
    for (const char* p = v; *p != '\0'; p++) {
      if (*p < '0' || *p > '9' || hz > kMaxHz) {
        *invalid = true;
        return 0u;
      }
      hz = hz * 10u + uint32_t(*p - '0');
    }
    if (hz == 0u || hz > kMaxHz) {
      *invalid = true;
      return 0u;
    }
    return hz;
  }


  /** A module named in the lines: its range and file name, looked up when first sampled. */
  struct Module {
    uintptr_t base;
    uintptr_t size;
    char      name[40];
  };


  /**
   * \brief The modules sampled so far
   *
   * Filled from the sampler thread only. A DLL unloaded and another one loaded at its address
   * keeps the first one's name: names are never looked up again.
   */
  class ModuleTable {

  public:

    /** The index of the module that holds \p addr, or -1. */
    int find(uintptr_t addr) const {
      for (uint32_t i = 0u; i < m_count; i++) {
        if (addr >= m_modules[i].base && addr - m_modules[i].base < m_modules[i].size)
          return int(i);
      }
      return -1;
    }

    /** Adds a module: its index, or -1 if the table is full, the range empty or it wraps. */
    int add(uintptr_t base, uintptr_t size, const char* name) {
      if (m_count >= kMaxModules || size == 0u || base + size < base)
        return -1;
      Module& m = m_modules[m_count];
      m.base = base;
      m.size = size;
      std::strncpy(m.name, name != nullptr ? name : "?", sizeof(m.name) - 1u);
      m.name[sizeof(m.name) - 1u] = '\0';
      return int(m_count++);
    }

    uint32_t count() const { return m_count; }

    const Module& at(uint32_t i) const { return m_modules[i]; }

  private:

    Module   m_modules[kMaxModules] = { };
    uint32_t m_count = 0u;

  };


  /** A window's samples. */
  struct Window {
    uint32_t perModule[kMaxModules] = { };
    uint32_t noModule     = 0u;   // outside every image, or in an image the table had no room for
    uint32_t missed       = 0u;   // SuspendThread or GetThreadContext failed
    uint64_t heldTotalNs  = 0u;   // the thread held suspended, from SuspendThread to ResumeThread
    uint64_t heldMaxNs    = 0u;

    uint32_t samples() const {
      uint32_t n = noModule;
      for (uint32_t c : perModule)
        n += c;
      return n;
    }

    /** One sample in the table's module \p index (-1: none), the thread held \p heldNs. */
    void add(int index, uint64_t heldNs) {
      if (index >= 0 && uint32_t(index) < kMaxModules)
        perModule[index]++;
      else
        noModule++;
      heldTotalNs += heldNs;
      if (heldNs > heldMaxNs)
        heldMaxNs = heldNs;
    }
  };


  /**
   * \brief The window's lines, without a line end
   *
   * The first: the thread, the window's number and length, the samples, the misses and how long
   * the thread was held suspended. Then one per module with samples, the most first (equal counts
   * in table order), and "(no module)" in that order too.
   */
  inline std::vector<std::string> windowLines(const ModuleTable& table, const Window& w,
      uint32_t tid, uint32_t number, double seconds, uint32_t hz, const char* why) {
    std::vector<std::string> lines;
    char buf[256];
    const uint32_t n = w.samples();
    const double meanMs = n != 0u ? double(w.heldTotalNs) / double(n) / 1.0e6 : 0.0;
    std::snprintf(buf, sizeof(buf),
      "D9MT-RT-SAMPLE: thread 0x%04x, window %u (%s), %.1f s, %u samples at %u Hz asked, %u missed, "
      "held suspended %.3f ms mean, %.3f ms longest",
      unsigned(tid), unsigned(number), why, seconds, unsigned(n), unsigned(hz), unsigned(w.missed),
      meanMs, double(w.heldMaxNs) / 1.0e6);
    lines.push_back(buf);

    // (count, index) pairs, index kMaxModules for "(no module)"; a stable insertion sort
    std::vector<std::pair<uint32_t, uint32_t>> rows;
    for (uint32_t i = 0u; i < table.count() && i < kMaxModules; i++) {
      if (w.perModule[i] != 0u)
        rows.emplace_back(w.perModule[i], i);
    }
    if (w.noModule != 0u)
      rows.emplace_back(w.noModule, kMaxModules);
    for (size_t i = 1u; i < rows.size(); i++) {
      for (size_t j = i; j > 0u && rows[j - 1u].first < rows[j].first; j--)
        std::swap(rows[j - 1u], rows[j]);
    }

    for (const auto& r : rows) {
      const char* name = r.second < kMaxModules ? table.at(r.second).name : "(no module)";
      std::snprintf(buf, sizeof(buf), "D9MT-RT-SAMPLE:   %5.1f %% %s (%u)",
        100.0 * double(r.first) / double(n), name, unsigned(r.first));
      lines.push_back(buf);
    }
    return lines;
  }


  /** The file name of a path: what follows the last '\\' or '/'. */
  inline const char* leafName(const char* path) {
    const char* leaf = path;
    for (const char* p = path; *p != '\0'; p++) {
      if (*p == '\\' || *p == '/')
        leaf = p + 1;
    }
    return leaf;
  }

}

#ifdef _WIN32
#include <windows.h>

namespace dxvk::rtsample {

  /** A running sampler, as its starter holds it: its own handles, nothing the sampler frees. */
  struct Handle {
    HANDLE stop   = nullptr;
    HANDLE thread = nullptr;
  };

  /**
   * \brief Starts a sampler of the calling thread, when D9MT_RT_SAMPLE asks for one
   *
   * Reads the variable; unset, returns an empty handle and does nothing else. Set but invalid:
   * one line says it is ignored. Otherwise one line names the thread and the rate. Each line goes
   * to \p sink, from the sampler's thread after the first.
   */
  Handle startFromEnv(void (*sink)(const char* line));

  /**
   * \brief Stops the sampler
   *
   * Asks it to write its last window and end. With \p wait, waits up to 500 ms for that (so a
   * program that exits right after has the lines). Never frees anything the sampler uses: the
   * sampler thread frees its own state. Closes the handle's handles and empties it.
   */
  void stop(Handle& h, bool wait);

  /**
   * \brief Tests only: starts a sampler of the calling thread at \p hz
   *
   * \p guard false turns the fault guard off (the watch second and the stop at a fault), for the
   * test that measures what the guard is for.
   */
  Handle startForTest(uint32_t hz, void (*sink)(const char* line), bool guard);

}
#endif
