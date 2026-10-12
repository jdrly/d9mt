#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dxvk {

  // Local wall-clock time "HH:MM:SS.mmm", for correlating log lines with
  // Instruments traces and other processes' logs.
  inline std::string wallClockStamp() {
    char buf[16] = { };
#ifdef _WIN32
    SYSTEMTIME t = { };
    GetLocalTime(&t);
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u",
      unsigned(t.wHour), unsigned(t.wMinute), unsigned(t.wSecond), unsigned(t.wMilliseconds));
#endif
    return buf;
  }

  // At most `perWindow` lines per `window`; counts what it drops so the next
  // allowed line can report it. Not thread-safe: one instance per thread (or
  // guard it).
  class LogLimiter {

  public:

    LogLimiter(uint32_t perWindow, std::chrono::seconds window)
    : m_perWindow(perWindow), m_window(window) { }

    bool allow(uint32_t* dropped) {
      auto now = std::chrono::steady_clock::now();

      if (now - m_start >= m_window) {
        m_start = now;
        m_count = 0u;
      }

      if (m_count >= m_perWindow) {
        m_dropped += 1u;
        return false;
      }

      m_count += 1u;
      *dropped  = m_dropped;
      m_dropped = 0u;
      return true;
    }

  private:

    uint32_t m_perWindow;
    std::chrono::steady_clock::duration m_window;
    std::chrono::steady_clock::time_point m_start = { };
    uint32_t m_count   = 0u;
    uint32_t m_dropped = 0u;

  };

}
