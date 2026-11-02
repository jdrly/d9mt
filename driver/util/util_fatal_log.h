#pragma once

#include <atomic>

// The fatal-exception log: a vectored exception handler that writes one `D9MT-FATAL` line per
// fatal exception (an NTSTATUS of the 0xC0000000 class: access violation, integer divide by zero,
// illegal instruction, stack overflow, …) to the log, before the program's own handlers see it.
//
// Why: a game that catches its own crash ends with the exception code as its exit code and
// nothing in the log (a player's M1 Pro: "exit code 148", the low byte of 0xC0000094). The line
// names the code, the address as module+RVA, the thread and the last d9mt step, so the next
// such report says where it happened.
//
// It never changes what happens: the handler always returns EXCEPTION_CONTINUE_SEARCH. It runs
// only when an exception is dispatched; the path without exceptions has no cost beyond the
// breadcrumbs below (one relaxed pointer store at a few cold places, none per draw).
// On by default; D9MT_FATAL_LOG=0 turns it off (docs/configuration.md).

namespace dxvk::fatal {

  /** The last d9mt step entered, on any thread: a static string, never freed. */
  inline std::atomic<const char*> g_breadcrumb = { nullptr };

  /**
   * \brief Marks the start of a d9mt step for the fatal-exception log
   *
   * One relaxed store. The string must be a literal (or otherwise live for the whole process):
   * the handler reads it while the program is crashing. It names the step entered last, which
   * may have finished since.
   */
  inline void breadcrumb(const char* step) {
    g_breadcrumb.store(step, std::memory_order_relaxed);
  }

#ifdef _WIN32
  /**
   * \brief Installs the handler once per process
   *
   * \returns \c true if the handler is installed (now or before), \c false if
   *   D9MT_FATAL_LOG=0 turned it off or AddVectoredExceptionHandler failed
   */
  bool install();

  /** Tests only: sends the lines to \p sink instead of the Wine debug output. */
  void setSinkForTest(void (*sink)(const char* line));
#endif

}
