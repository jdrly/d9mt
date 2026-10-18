#pragma once

// D9MT_PROFILE: how often a game uses StretchRect and ColorFill, by path,
// for test sessions (#14: these paths did nothing before). The app thread
// counts under the device lock; after the STATS line, an XFER line reports
// the window's counts when there were any:
//
//   XFER t=63 | stretch copy=2 resolve=0 blit=5 (scaled=5 linear=4 convert=1 msaa=0) skip=0
//     | fill rt=1 rect=1 bc=0 skip=0
//
//   copy/resolve  StretchRect's direct copy or MSAA resolve (same size, similar formats)
//   blit          a filtered draw: scaled (sizes differ), linear (scaled with
//                 D3DTEXF_LINEAR or better), convert (formats not similar),
//                 msaa (multisampled source, resolved first)
//   stretch skip  blits left alone: the destination can't be rendered to
//   rt/rect       ColorFill of an uncompressed surface: whole / part of it
//   bc            ColorFill of a block-compressed surface (BC1-3, DXT1-5)
//   fill skip     ColorFills left alone: ATI1/ATI2, or an uncompressed
//                 surface that can't be rendered to (a texture without
//                 D3DUSAGE_RENDERTARGET)
//
// XFER-SUMMARY with the session's totals when the device is destroyed.
// Off (D9MT_PROFILE unset): nothing is counted. Header-only and
// dependency-free (test/native/xfer_stats_test.cpp).

#include <cstdint>
#include <cstdio>
#include <string>

namespace dxvk::xfer {

  enum Counter : uint32_t {
    StretchCopy = 0,
    StretchResolve,
    StretchBlit,
    BlitScaled,
    BlitLinear,
    BlitConvert,
    BlitMsaa,
    StretchSkip,
    FillRt,
    FillRect,
    FillBc,
    FillSkip,
    Count
  };

  struct Counts {
    uint64_t v[Count] = { };
  };

  inline std::string format(const char* head, const Counts& c) {
    char buf[320];
    std::snprintf(buf, sizeof(buf),
      "%s | stretch copy=%llu resolve=%llu blit=%llu (scaled=%llu linear=%llu convert=%llu msaa=%llu) skip=%llu"
      " | fill rt=%llu rect=%llu bc=%llu skip=%llu", head,
      (unsigned long long)c.v[StretchCopy], (unsigned long long)c.v[StretchResolve],
      (unsigned long long)c.v[StretchBlit], (unsigned long long)c.v[BlitScaled],
      (unsigned long long)c.v[BlitLinear], (unsigned long long)c.v[BlitConvert],
      (unsigned long long)c.v[BlitMsaa], (unsigned long long)c.v[StretchSkip],
      (unsigned long long)c.v[FillRt], (unsigned long long)c.v[FillRect],
      (unsigned long long)c.v[FillBc], (unsigned long long)c.v[FillSkip]);
    return buf;
  }

  // Totals since the device was created, and the part not yet reported.
  class Stats {

  public:

    void add(Counter c) {
      m_total.v[c]++;
    }

    // The XFER line for the window since the last one; empty when nothing happened.
    std::string take(double t) {
      Counts d;
      bool any = false;
      for (uint32_t i = 0u; i < Count; i++) {
        d.v[i] = m_total.v[i] - m_reported.v[i];
        any = any || d.v[i] != 0u;
      }
      m_reported = m_total;
      if (!any)
        return std::string();
      char head[32];
      std::snprintf(head, sizeof(head), "XFER t=%.0f", t);
      return format(head, d);
    }

    std::string summary() const {
      return format("XFER-SUMMARY", m_total);
    }

  private:

    Counts m_total;
    Counts m_reported;

  };

}
