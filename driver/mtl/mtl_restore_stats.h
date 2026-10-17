#pragma once

// D9MT_PROFILE: what made the attachment load/store optimization re-clear an
// attachment ("restore", backend/mtl_attachment_opt.h). A target whose clear
// was never stored (Lazy) is re-cleared in memory before anything outside a
// render pass reads it; each restore is one extra render encoder.
//
// The render (CS) thread adds one key per restore: the image (Metal pixel
// format, size), the aspects re-cleared, why (the backend operation that read
// it) and which D3D9 call that operation came from (the frontend tags the CS
// stream with setApiTag while D9MT_PROFILE is set). The app thread logs
//
//   RESTORE t=63 frames=60 n=128 perFrame=2.13 | 60x 2048x2048 Depth32Float_Stencil8 s
//     sample@Draw | 60x 3840x2160 BGRA8Unorm c copy@StretchRect | ...
//
// once a second when the second had any (the five most frequent keys, the rest
// summed up as "+N other"), and a RESTORE-SUMMARY line with the session's ten
// most frequent keys when the device is destroyed.
//
// Header-only and dependency-free (test/native/stats_test.cpp).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace dxvk::rstore {

  // The backend operation that needed the attachment's contents in memory.
  enum class Reason : uint8_t {
    Sample,        // bound for sampling
    Copy,          // copy source (copyTexture)
    CopyDst,       // partial copy destination (the rest must survive)
    Blit,          // blitTextureView source or destination (filtered StretchRect, Present)
    Resolve,       // MSAA resolve source or destination
    Readback,      // copied to a buffer (GetRenderTargetData, LockRect, dumps)
    Upload,        // buffer-to-texture upload destination
    PartialClear,  // partial clear (rects, viewport or scissor smaller than the target)
    MipGen,        // mipmap generation
    RtView,        // rendered through a view the optimization doesn't track (other format)
    Present,       // MetalFX upscale at Present
    Other,
    Count
  };

  // The D3D9 call the backend operation came from (the last tagged one).
  enum class Api : uint8_t {
    Unknown,
    Draw,
    Clear,
    StretchRect,
    ColorFill,
    UpdateSurface,
    UpdateTexture,
    GetRenderTargetData,
    GetFrontBufferData,
    LockRect,
    ResolveZ,
    Present,
    Count
  };

  // Aspect bits (MtlAspectColor/Depth/Stencil).
  constexpr uint8_t AspectColor = 1u, AspectDepth = 2u, AspectStencil = 4u;

  inline const char* reasonName(Reason r) {
    static const char* const k[] = { "sample", "copy", "copy-dst", "blit", "resolve", "readback",
      "upload", "partial-clear", "mipgen", "rt-view", "present", "other" };
    return uint32_t(r) < uint32_t(Reason::Count) ? k[uint32_t(r)] : "other";
  }

  inline const char* apiName(Api a) {
    static const char* const k[] = { "Unknown", "Draw", "Clear", "StretchRect", "ColorFill",
      "UpdateSurface", "UpdateTexture", "GetRenderTargetData", "GetFrontBufferData", "LockRect",
      "ResolveZ", "Present" };
    return uint32_t(a) < uint32_t(Api::Count) ? k[uint32_t(a)] : "Unknown";
  }

  // Metal pixel format names of the usual render target formats (MtlFormat
  // values are MTLPixelFormat values); others print as their number.
  inline std::string formatName(uint32_t f) {
    switch (f) {
      case 1u:   return "A8Unorm";
      case 10u:  return "R8Unorm";
      case 25u:  return "R16Float";
      case 30u:  return "RG8Unorm";
      case 40u:  return "B5G6R5Unorm";
      case 55u:  return "R32Float";
      case 60u:  return "RG16Unorm";
      case 65u:  return "RG16Float";
      case 70u:  return "RGBA8Unorm";
      case 71u:  return "RGBA8Unorm_sRGB";
      case 80u:  return "BGRA8Unorm";
      case 81u:  return "BGRA8Unorm_sRGB";
      case 90u:  return "RGB10A2Unorm";
      case 94u:  return "BGR10A2Unorm";
      case 105u: return "RG32Float";
      case 110u: return "RGBA16Unorm";
      case 115u: return "RGBA16Float";
      case 125u: return "RGBA32Float";
      case 250u: return "Depth16Unorm";
      case 252u: return "Depth32Float";
      case 253u: return "Stencil8";
      case 255u: return "Depth24Unorm_Stencil8";
      case 260u: return "Depth32Float_Stencil8";
      default:   return "fmt" + std::to_string(f);
    }
  }

  inline std::string aspectName(uint8_t a) {
    std::string s;
    if (a & AspectColor)   s += 'c';
    if (a & AspectDepth)   s += 'd';
    if (a & AspectStencil) s += 's';
    return s.empty() ? "-" : s;
  }

  struct Key {
    uint32_t format  = 0u;
    uint32_t width   = 0u;
    uint32_t height  = 0u;
    uint8_t  aspects = 0u;   // the aspects re-cleared
    Reason   reason  = Reason::Other;
    Api      api     = Api::Unknown;

    bool operator == (const Key& o) const {
      return format == o.format && width == o.width && height == o.height
          && aspects == o.aspects && reason == o.reason && api == o.api;
    }
  };

  struct Entry {
    Key      key;
    uint64_t count = 0u;
  };

  using Entries = std::vector<Entry>;

  inline std::string describe(const Key& k) {
    return std::to_string(k.width) + "x" + std::to_string(k.height) + " " + formatName(k.format)
         + " " + aspectName(k.aspects) + " " + reasonName(k.reason) + "@" + apiName(k.api);
  }

  // Session totals per key. add() runs on the render thread (one call per
  // restore, which already costs a render encoder); snapshot() on the app
  // thread. Bounded: keys beyond MaxKeys count as one overflow key.
  class Log {

  public:

    static constexpr size_t MaxKeys = 256u;

    void add(const Key& k) {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (bump(k))
        return;
      if (m_entries.size() < MaxKeys - 1u) {
        m_entries.push_back({ k, 1u });
        return;
      }
      // Full: every new key counts as one default (0x0 other@Unknown) key.
      const Key other;
      if (!bump(other))
        m_entries.push_back({ other, 1u });
    }

    Entries snapshot() const {
      std::lock_guard<std::mutex> lock(m_mutex);
      return m_entries;
    }

  private:

    bool bump(const Key& k) {
      for (Entry& e : m_entries) {
        if (e.key == k) {
          e.count++;
          return true;
        }
      }
      return false;
    }


    mutable std::mutex m_mutex;
    Entries            m_entries;

  };

  namespace detail {

    inline uint64_t total(const Entries& e) {
      uint64_t n = 0u;
      for (const Entry& x : e)
        n += x.count;
      return n;
    }

    // " | 60x <key> | ... | +N other": the `top` largest, most frequent first.
    inline std::string list(Entries e, size_t top) {
      std::stable_sort(e.begin(), e.end(), [] (const Entry& a, const Entry& b) { return a.count > b.count; });
      std::string s;
      uint64_t rest = 0u;
      for (size_t i = 0u; i < e.size(); i++) {
        if (i < top)
          s += " | " + std::to_string(e[i].count) + "x " + describe(e[i].key);
        else
          rest += e[i].count;
      }
      if (rest)
        s += " | +" + std::to_string(rest) + " other";
      return s;
    }

    inline std::string perFrame(uint64_t n, uint64_t frames) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.2f", frames ? double(n) / double(frames) : 0.0);
      return buf;
    }

  }

  // The once-a-second RESTORE line, from the session totals; empty when
  // nothing was restored since the last call.
  class Window {

  public:

    // `t`: seconds since the first Present; `framesTotal`: render-thread
    // frames so far (rstats::Frames).
    std::string take(double t, uint64_t framesTotal, const Entries& cur, size_t top = 5u) {
      Entries d;
      for (const Entry& e : cur) {
        uint64_t before = 0u;
        for (const Entry& p : m_prev) {
          if (p.key == e.key) {
            before = p.count;
            break;
          }
        }
        if (e.count > before)
          d.push_back({ e.key, e.count - before });
      }
      const uint64_t frames = framesTotal - m_prevFrames;
      m_prev = cur;
      m_prevFrames = framesTotal;
      if (d.empty())
        return std::string();

      const uint64_t n = detail::total(d);
      char head[96];
      std::snprintf(head, sizeof(head), "RESTORE t=%.0f frames=%llu n=%llu perFrame=",
        t, (unsigned long long)frames, (unsigned long long)n);
      return head + detail::perFrame(n, frames) + detail::list(std::move(d), top);
    }

  private:

    Entries  m_prev;
    uint64_t m_prevFrames = 0u;

  };

  // The session's RESTORE-SUMMARY line (also when nothing was restored).
  inline std::string summary(uint64_t frames, const Entries& cur, size_t top = 10u) {
    const uint64_t n = detail::total(cur);
    char head[96];
    std::snprintf(head, sizeof(head), "RESTORE-SUMMARY frames=%llu n=%llu perFrame=",
      (unsigned long long)frames, (unsigned long long)n);
    return head + detail::perFrame(n, frames) + detail::list(cur, top);
  }

}
