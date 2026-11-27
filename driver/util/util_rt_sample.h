#pragma once

// D9MT_RT_SAMPLE=<Hz>: a sampler of the render thread, the thread that created the device. A
// developer switch, off by default; no app sets it.
//
// Why: a game's render thread spends its time in the game's own code, in DLLs the game calls
// (d3dx9, the C runtime) and in d9mt. PACE measures d9mt's part from inside d9mt; nothing measured
// the rest. A native profiler of the Mac can't help: under Rosetta its stacks lose the x86 leaf.
//
// What it does: a thread of its own suspends the render thread at the rate asked, reads its
// instruction pointer and the top of its stack (SuspendThread, GetThreadContext, a copy of at most
// kStackSlots stack slots, ResumeThread), and maps the address to the module that holds it, from a
// table of the loaded modules it makes again at each window's start. Every 10 s, and when the
// device is released, it writes:
//   - one line with the window's samples, misses and how long the thread was held suspended;
//   - one line per module with its share of the samples, the most first. A sample in a system
//     DLL (ntdll, kernelbase, the C runtimes, ...: isLeafName) also names its caller: the first
//     stack slot that points into an executable section of a module that isn't one, so a d3dx9
//     call that spends its time in the C runtime's strcmp counts as "ucrtbase.dll, called from
//     d3dx9_43.dll". A heuristic: a stale return address can stand in for the real caller;
//   - the samples inside one module (kRvaModule, d3dx9_43.dll) by RVA, in 16-byte buckets, and
//     the RVAs of that module's caller slots, so a function map of the DLL can sum them.
// Shares are of wall time: a wait counts too, under the module the thread waits in (ntdll.dll for
// most). A thread inside a Unix call (Metal, the wineserver) shows the PE caller's address:
// winemetal.dll for d9mt's Metal calls, ntdll.dll or win32u.dll for Wine's system calls.
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
//   - between the suspend and the resume it lets a capture thread of its own call
//     GetThreadContext and waits for it at most 250 ms (Wine's call waits without a limit for the
//     thread to stop; past the limit the sampler resumes the thread and stops for good), and
//     copies stack slots only between the stack pointer and the base of the thread's own stack as
//     it was at the start (memory the thread has used; a thread on a fiber's stack isn't read).
//     No heap, no lock, no module lookup, no log: a suspended thread may hold any of them.
//     D9MT_RT_SAMPLE_STACK=0 turns the stack copy off (no callers then): a live stack page that
//     another thread made unreadable would fault the copy while the thread is suspended.
// What is left: a first fault that comes while a suspend is in flight; a fault that a vectored
// handler added later as first resolves, and one Wine resolves itself, are never seen.
//
// The pure part (the parse, the module table, the caller search, the lines) is in this header, so
// it builds natively (test/native/rt_sample_test.cpp); the sampler itself is util_rt_sample.cpp
// (Windows only).

#include <algorithm>
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
  constexpr uint32_t kMaxModules = 256u;    // modules named in the lines; more go to "(no module)"
  constexpr uint32_t kMaxLeaves  = 16u;     // system DLLs whose samples name a caller
  constexpr uint32_t kStackSlots = 64u;     // stack slots searched for a caller
  constexpr uint32_t kRvaBucket  = 16u;     // bytes per RVA bucket
  constexpr uintptr_t kMaxRvaImage = 16u << 20;  // a larger kRvaModule gets no RVA lines

  /** The module whose samples are also written by RVA. */
  constexpr const char* kRvaModule = "d3dx9_43.dll";

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


  /** ASCII case-insensitive equality of two names. */
  inline bool sameName(const char* a, const char* b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
      const char x = (*a >= 'A' && *a <= 'Z') ? char(*a + 32) : *a;
      const char y = (*b >= 'A' && *b <= 'Z') ? char(*b + 32) : *b;
      if (x != y)
        return false;
    }
    return *a == *b;
  }


  /** A system DLL: a sample in it names the caller found on the stack. */
  inline bool isLeafName(const char* name) {
    static const char* const names[] = {
      "ntdll.dll", "win32u.dll", "kernelbase.dll", "kernel32.dll", "user32.dll", "gdi32.dll",
      "ucrtbase.dll", "msvcrt.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
    };
    for (const char* n : names) {
      if (sameName(name, n))
        return true;
    }
    return false;
  }


  constexpr uint32_t kMaxCodeRanges = 16u;

  /** An address range, absolute: [begin, end). */
  struct Range {
    uintptr_t begin;
    uintptr_t end;
  };

  /** A module named in the lines: its range, its executable sections and its file name. */
  struct Module {
    uintptr_t base;
    uintptr_t size;
    Range     code[kMaxCodeRanges];   // its executable sections (the first kMaxCodeRanges)
    uint32_t  codeRanges;
    int       leaf;                   // its index among the system DLLs, -1 for any other module
    char      name[40];

    bool inCode(uintptr_t addr) const {
      for (uint32_t i = 0u; i < codeRanges; i++) {
        if (addr >= code[i].begin && addr < code[i].end)
          return true;
      }
      return false;
    }
  };


  /**
   * \brief The modules seen so far
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

    /**
     * Adds a module: its index, or -1 if the table is full, the range empty or it wraps. Of its
     * \p nCode executable ranges the first kMaxCodeRanges inside the image are kept (none: no
     * caller is ever found in the module). The 17th system DLL and later are added as other
     * modules.
     */
    int add(uintptr_t base, uintptr_t size, const char* name, const Range* code = nullptr, uint32_t nCode = 0u) {
      if (m_count >= kMaxModules || size == 0u || base + size < base)
        return -1;
      Module& m = m_modules[m_count];
      m.base = base;
      m.size = size;
      m.codeRanges = 0u;
      for (uint32_t i = 0u; i < nCode && m.codeRanges < kMaxCodeRanges; i++) {
        if (code[i].begin >= base && code[i].begin < code[i].end && code[i].end - base <= size)
          m.code[m.codeRanges++] = code[i];
      }
      std::strncpy(m.name, name != nullptr ? name : "?", sizeof(m.name) - 1u);
      m.name[sizeof(m.name) - 1u] = '\0';
      m.leaf = (isLeafName(m.name) && m_leaves < kMaxLeaves) ? int(m_leaves++) : -1;
      return int(m_count++);
    }

    /**
     * The caller of a sample in a system DLL: the module of the first of \p n stack slots that
     * points into an executable section of a module that isn't a system DLL, or -1.
     */
    int findCaller(const uintptr_t* slots, uint32_t n, uint32_t* slot = nullptr) const {
      for (uint32_t s = 0u; s < n; s++) {
        const int i = find(slots[s]);
        if (i >= 0 && m_modules[i].leaf < 0 && m_modules[i].inCode(slots[s])) {
          if (slot != nullptr)
            *slot = s;
          return i;
        }
      }
      return -1;
    }

    uint32_t count() const { return m_count; }

    const Module& at(uint32_t i) const { return m_modules[i]; }

  private:

    Module   m_modules[kMaxModules] = { };
    uint32_t m_count  = 0u;
    uint32_t m_leaves = 0u;

  };


  /** A window's samples. */
  struct Window {
    uint32_t perModule[kMaxModules] = { };
    uint32_t noModule     = 0u;   // outside every image, or in an image the table had no room for
    uint32_t missed       = 0u;   // SuspendThread or GetThreadContext failed
    uint64_t heldTotalNs  = 0u;   // the thread held suspended, from SuspendThread to ResumeThread
    uint64_t heldMaxNs    = 0u;

    // per system DLL: its samples by caller module, and those with no caller found and with the
    // stack not read (the stack pointer outside the thread's stack range)
    uint32_t callers[kMaxLeaves][kMaxModules] = { };
    uint32_t callerNone[kMaxLeaves]  = { };
    uint32_t stackUnread[kMaxLeaves] = { };

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

    /** A sample of system DLL \p leaf: its caller's module index, -1 none found, -2 not read. */
    void addCaller(int leaf, int caller) {
      if (leaf < 0 || uint32_t(leaf) >= kMaxLeaves)
        return;
      if (caller >= 0 && uint32_t(caller) < kMaxModules)
        callers[leaf][caller]++;
      else if (caller == -2)
        stackUnread[leaf]++;
      else
        callerNone[leaf]++;
    }
  };


  /** (count, index) pairs of \p counts' nonzero entries, the most first, equal counts by index. */
  inline std::vector<std::pair<uint32_t, uint32_t>> ranked(const uint32_t* counts, uint32_t n) {
    std::vector<std::pair<uint32_t, uint32_t>> rows;
    for (uint32_t i = 0u; i < n; i++) {
      if (counts[i] != 0u)
        rows.emplace_back(counts[i], i);
    }
    for (size_t i = 1u; i < rows.size(); i++) {
      for (size_t j = i; j > 0u && rows[j - 1u].first < rows[j].first; j--)
        std::swap(rows[j - 1u], rows[j]);
    }
    return rows;
  }


  /**
   * \brief The window's lines, without a line end
   *
   * The first: the thread, the window's number and length, the samples, the misses and how long
   * the thread was held suspended. Then one per module with samples, the most first (equal counts
   * in table order), "(no module)" among them; a system DLL's line goes on with its callers (the
   * six most), and its samples with no caller found or with the stack not read.
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

    // kMaxModules + 1 counts: the modules', then "(no module)"
    std::vector<uint32_t> counts(w.perModule, w.perModule + kMaxModules);
    for (uint32_t i = table.count(); i < kMaxModules; i++)
      counts[i] = 0u;
    counts.push_back(w.noModule);

    for (const auto& r : ranked(counts.data(), uint32_t(counts.size()))) {
      const Module* m = r.second < kMaxModules ? &table.at(r.second) : nullptr;
      std::snprintf(buf, sizeof(buf), "D9MT-RT-SAMPLE:   %5.1f %% %s (%u)",
        100.0 * double(r.first) / double(n), m != nullptr ? m->name : "(no module)", unsigned(r.first));
      std::string line = buf;

      if (m != nullptr && m->leaf >= 0) {
        const uint32_t leaf = uint32_t(m->leaf);
        const auto callers = ranked(w.callers[leaf], table.count());
        for (size_t c = 0u; c < callers.size() && c < 6u; c++) {
          std::snprintf(buf, sizeof(buf), "%s %s %u", c == 0u ? ", called from" : ",",
            table.at(callers[c].second).name, unsigned(callers[c].first));
          line += buf;
        }
        uint32_t rest = 0u;
        for (size_t c = 6u; c < callers.size(); c++)
          rest += callers[c].first;
        if (rest != 0u) {
          std::snprintf(buf, sizeof(buf), ", other modules %u", unsigned(rest));
          line += buf;
        }
        std::snprintf(buf, sizeof(buf), ", no caller found %u, stack not read %u",
          unsigned(w.callerNone[leaf]), unsigned(w.stackUnread[leaf]));
        line += buf;
      }
      lines.push_back(line);
    }
    return lines;
  }


  /** Samples in one module by RVA, in kRvaBucket-byte buckets. */
  class RvaHistogram {

  public:

    /** Sizes it for a module of \p size bytes; false (and empty) if it is over kMaxRvaImage. */
    bool init(uintptr_t size) {
      m_counts.clear();
      if (size == 0u || size > kMaxRvaImage)
        return false;
      m_counts.assign((size + kRvaBucket - 1u) / kRvaBucket, 0u);
      return true;
    }

    bool empty() const { return m_counts.empty(); }

    /** One sample at \p rva; outside the module: nothing. A bucket saturates at 65535. */
    void add(uintptr_t rva) {
      const uintptr_t b = rva / kRvaBucket;
      if (b < m_counts.size() && m_counts[b] != 0xffffu)
        m_counts[b]++;
    }

    void clear() {
      std::fill(m_counts.begin(), m_counts.end(), uint16_t(0u));
    }

    /**
     * The nonzero buckets as "<rva>=<count>" (the bucket's first RVA, hex), 24 per line, by RVA:
     * "D9MT-RT-SAMPLE: <module> <what> window <n>: 1c6b0=12 1c6c0=3 ...". None: no line.
     */
    std::vector<std::string> lines(const char* module, const char* what, uint32_t number) const {
      std::vector<std::string> out;
      char buf[64];
      std::string line;
      uint32_t inLine = 0u;
      for (size_t b = 0u; b < m_counts.size(); b++) {
        if (m_counts[b] == 0u)
          continue;
        if (inLine == 0u) {
          std::snprintf(buf, sizeof(buf), "window %u:", unsigned(number));
          line = std::string("D9MT-RT-SAMPLE: ") + module + " " + what + " " + buf;
        }
        std::snprintf(buf, sizeof(buf), " %llx=%u", (unsigned long long)(b * kRvaBucket), unsigned(m_counts[b]));
        line += buf;
        if (++inLine == 24u) {
          out.push_back(line);
          inLine = 0u;
        }
      }
      if (inLine != 0u)
        out.push_back(line);
      return out;
    }

  private:

    std::vector<uint16_t> m_counts;

  };


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
   * Reads the variable (and D9MT_RT_SAMPLE_STACK when it is set); unset, returns an empty
   * handle and does nothing else. Set but invalid:
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
   * test that measures what the guard is for. \p stack false turns the stack copy off.
   * \p rvaModule names the module written by RVA in place of kRvaModule.
   */
  Handle startForTest(uint32_t hz, void (*sink)(const char* line), bool guard, bool stack, const char* rvaModule);

}
#endif
