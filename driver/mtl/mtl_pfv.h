#pragma once

#include <cstdint>
#include <string>

namespace dxvk {

  // Which images get MTLTextureUsagePixelFormatView, which turns lossless compression off
  // (MtlDeviceInfo::pfvImages; D9MT_TEX_PFV). Block-compressed images always get it.
  //
  // Lossless compression of 4x MSAA images and of the images MSAA resolves write drew
  // flickering lines on an M4 (Apple9); D9MT_TEX_PFV=1 (every image) removed them. On GPUs
  // before Apple10 (M1-M4) those images get PixelFormatView by default. The M5 (Apple10) has
  // new compression hardware, shows nothing, and keeps compression.
  namespace MtlPfvImages {
    constexpr uint32_t MsaaColor    = 1u << 0;  // colour images with sampleCount > 1
    constexpr uint32_t MsaaDepth    = 1u << 1;  // depth-stencil images with sampleCount > 1
    constexpr uint32_t ResolveColor = 1u << 2;  // colour images only MSAA resolves write
    constexpr uint32_t ResolveDepth = 1u << 3;  // depth-stencil images only MSAA resolves write
    constexpr uint32_t Every        = 1u << 4;  // every image (D9MT_TEX_PFV=1)
    constexpr uint32_t Msaa         = MsaaColor | MsaaDepth;
    constexpr uint32_t Resolve      = ResolveColor | ResolveDepth;
    constexpr uint32_t Depth        = MsaaDepth | ResolveDepth;
    constexpr uint32_t Default      = ~0u;      // parse(): no setting, the GPU's default

    // D9MT_TEX_PFV's value:
    //   unset or empty: Default
    //   a value starting with 1: Every (as before #18)
    //   0: none, on every GPU
    //   msaa, resolve, ds, or a comma list of them: those sets, on every GPU
    // Anything else is Default with understood = false.
    inline uint32_t parse(const std::string& value, bool& understood) {
      understood = true;
      if (value.empty())
        return Default;
      if (value[0] == '1')
        return Every;
      if (value == "0")
        return 0u;
      uint32_t set = 0u;
      for (size_t pos = 0u; pos <= value.size(); ) {
        size_t end = value.find(',', pos);
        if (end == std::string::npos)
          end = value.size();
        const std::string part = value.substr(pos, end - pos);
        if (part == "msaa")
          set |= Msaa;
        else if (part == "resolve")
          set |= Resolve;
        else if (part == "ds")
          set |= Depth;
        else {
          understood = false;
          return Default;
        }
        pos = end + 1u;
      }
      return set;
    }

    // The set in effect: the setting, or the GPU's default.
    inline uint32_t effective(uint32_t setting, bool beforeApple10) {
      if (setting != Default)
        return setting;
      return beforeApple10 ? Msaa | Resolve : 0u;
    }

    // Does an image get PixelFormatView?
    inline bool wants(uint32_t pfv, bool blockCompressed, bool depth,
                      uint32_t sampleCount, bool resolveTarget) {
      if ((pfv & Every) || blockCompressed)
        return true;
      if (sampleCount > 1u)
        return (pfv & (depth ? MsaaDepth : MsaaColor)) != 0u;
      if (resolveTarget)
        return (pfv & (depth ? ResolveDepth : ResolveColor)) != 0u;
      return false;
    }

    // Does an MSAA resolve that writes a game's image (which keeps lossless compression)
    // go through the source's resolve image (which doesn't) and a copy instead?
    inline bool resolveViaImage(uint32_t pfv, bool depth) {
      return !(pfv & Every) && (pfv & (depth ? ResolveDepth : ResolveColor)) != 0u;
    }
  }

}
