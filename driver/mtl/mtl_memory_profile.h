#pragma once

// Memory profiles (MEMORY-PLAN.md §3). Header-only and dependency-free so the
// tier table can be unit-tested natively (test/native/memory_profile_test.cpp);
// config/env resolution lives in mtl_memory_profile.cpp.

#include <cstdint>
#include <string>

namespace dxvk {

  class Config;

  enum class MtlMemoryTier : uint32_t { Low, Medium, High, Ultra, Unlimited };

  struct MtlMemoryProfile {
    MtlMemoryTier tier           = MtlMemoryTier::Unlimited;
    bool          autoTier       = false;
    uint64_t      physRam        = 0u;   // bytes, 0 = unknown
    uint64_t      budget         = 0u;   // soft Metal budget in bytes, 0 = none (warning/LRU: later phases)
    uint32_t      reportedVramMB = 4096u; // d3d9 GetAvailableTextureMem cap unless d3d9.maxAvailableMemory is set
    // `unlimited` = the pre-profile driver in every respect, for A/B runs:
    // no staging-block pool, one residency commit per change, FIFO ring that
    // keeps oversized chunks, no MetalFX scaler eviction, buffer in-use
    // tracking not reset on DISCARD renames.
    bool          legacy         = false;
    bool          writeOnlyDirect = true; // DEFAULT+WRITEONLY buffers mapped directly (d9mt.writeOnlyDirect)
    // Later phases (not consumed yet): managed/BUFFER-mode CPU copy eviction, LRU residency.
    bool          evictManagedTex    = false;
    bool          evictBufferStaging = false;
    bool          residencyLru       = false;
    uint32_t      coldFrames         = 0u;
    // Pools.
    uint32_t      stagingBlocks      = 8u;   // idle 4 MiB D3D9 staging blocks kept for reuse (0 = no pool)
    uint32_t      stagingInFlightMB  = 90u;  // D3D9 staging bytes in flight before the CPU waits
    uint32_t      recycledPerBuffer  = 4u;   // spare DISCARD backings kept per buffer
    uint64_t      recycledCapBytes   = 0u;   // device-wide cap on spare backings, 0 = none
    uint32_t      ringKeep           = 0u;   // idle upload-ring chunks kept, 0 = all
    uint32_t      allocatorCap       = 32u;  // command allocators per context
    // Diagnostics.
    uint32_t      statsInterval      = 0u;   // DIAG MEM line every N frames, 0 = off
    bool          labels             = false;
  };

  inline const char* memoryTierName(MtlMemoryTier t) {
    switch (t) {
      case MtlMemoryTier::Low:       return "low";
      case MtlMemoryTier::Medium:    return "medium";
      case MtlMemoryTier::High:      return "high";
      case MtlMemoryTier::Ultra:     return "ultra";
      default:                       return "unlimited";
    }
  }

  // Env/config switch: 1 = on, 0 = off, -1 = unset or unrecognised.
  inline int parseSwitch(std::string s) {
    for (char& c : s)
      c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    if (s == "1" || s == "true" || s == "yes" || s == "on")   return 1;
    if (s == "0" || s == "false" || s == "no" || s == "off")  return 0;
    return -1;
  }

  // "auto" (or empty) -> isAuto. Returns false for unknown names.
  inline bool parseMemoryTier(std::string s, MtlMemoryTier& tier, bool& isAuto) {
    for (char& c : s)
      c = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    isAuto = s.empty() || s == "auto";
    if (isAuto)                return true;
    if (s == "low")            tier = MtlMemoryTier::Low;
    else if (s == "medium")    tier = MtlMemoryTier::Medium;
    else if (s == "high")      tier = MtlMemoryTier::High;
    else if (s == "ultra")     tier = MtlMemoryTier::Ultra;
    else if (s == "unlimited") tier = MtlMemoryTier::Unlimited;
    else                       return false;
    return true;
  }

  // Installed RAM -> tier. Half a GiB of slack because the OS may report a
  // little less than the nominal size; 0 (unknown) keeps today's behaviour.
  inline MtlMemoryTier autoMemoryTier(uint64_t physRam) {
    constexpr uint64_t GiB = 1ull << 30;
    if (!physRam)                        return MtlMemoryTier::Unlimited;
    if (physRam <= 8u  * GiB + GiB / 2u) return MtlMemoryTier::Low;
    if (physRam <= 16u * GiB + GiB / 2u) return MtlMemoryTier::Medium;
    if (physRam <= 24u * GiB + GiB / 2u) return MtlMemoryTier::High;
    if (physRam <  63u * GiB + GiB / 2u) return MtlMemoryTier::Ultra;
    return MtlMemoryTier::Unlimited;
  }

  // The tier table. `unlimited` is exactly the pre-profile behaviour. Until
  // MEMSTATS data says otherwise, `ultra` keeps unlimited's pool sizes and
  // caps; it differs in the budget, the `legacy` switches and, later, in
  // managed-texture eviction.
  inline MtlMemoryProfile memoryProfileForTier(MtlMemoryTier tier) {
    constexpr uint64_t MiB = 1ull << 20;
    MtlMemoryProfile p;
    p.tier = tier;
    switch (tier) {
      case MtlMemoryTier::Low:
        p.budget = 2560u * MiB; p.reportedVramMB = 1024u;
        p.evictManagedTex = p.evictBufferStaging = p.residencyLru = true; p.coldFrames = 300u;
        p.stagingBlocks = 2u; p.stagingInFlightMB = 30u;
        p.recycledPerBuffer = 1u; p.recycledCapBytes = 32u * MiB;
        p.ringKeep = 1u; p.allocatorCap = 8u;
        break;
      case MtlMemoryTier::Medium:
        p.budget = 5120u * MiB; p.reportedVramMB = 2048u;
        p.evictManagedTex = p.evictBufferStaging = p.residencyLru = true; p.coldFrames = 600u;
        p.stagingBlocks = 4u; p.stagingInFlightMB = 60u;
        p.recycledPerBuffer = 2u; p.recycledCapBytes = 64u * MiB;
        p.ringKeep = 2u; p.allocatorCap = 12u;
        break;
      case MtlMemoryTier::High:
        p.budget = 8192u * MiB; p.reportedVramMB = 3072u;
        p.evictManagedTex = p.evictBufferStaging = true;
        p.stagingBlocks = 6u; p.stagingInFlightMB = 90u;
        p.recycledPerBuffer = 4u; p.recycledCapBytes = 128u * MiB;
        p.ringKeep = 4u; p.allocatorCap = 16u;
        break;
      case MtlMemoryTier::Ultra:
        p.budget = 16384u * MiB;
        p.evictManagedTex = true;
        break;
      case MtlMemoryTier::Unlimited:
        p.legacy = true;
        p.writeOnlyDirect = false;
        p.stagingBlocks = 0u;
        break;
    }
    return p;
  }

  // Tier from config/env/RAM plus explicit overrides (d9mt.* options, D9MT_* env).
  MtlMemoryProfile resolveMemoryProfile(const Config& config);

  // Installed RAM in bytes (GlobalMemoryStatusEx), 0 if unknown.
  uint64_t queryPhysicalRam();

}
