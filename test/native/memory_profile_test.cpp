// Native unit test for the memory profile tier table (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/mtl test/native/memory_profile_test.cpp -o /tmp/mpt && /tmp/mpt
#include "mtl_memory_profile.h"

#include <cstdio>
#include <cstring>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
  constexpr uint64_t GiB = 1ull << 30, MiB = 1ull << 20;

  // Auto tiers from installed RAM (nominal sizes, and slightly under/over).
  CHECK(autoMemoryTier(0) == MtlMemoryTier::Unlimited);
  CHECK(autoMemoryTier(8 * GiB) == MtlMemoryTier::Low);
  CHECK(autoMemoryTier(8 * GiB - 300 * MiB) == MtlMemoryTier::Low);
  CHECK(autoMemoryTier(8 * GiB + 300 * MiB) == MtlMemoryTier::Low);
  CHECK(autoMemoryTier(16 * GiB) == MtlMemoryTier::Medium);
  CHECK(autoMemoryTier(18 * GiB) == MtlMemoryTier::High);
  CHECK(autoMemoryTier(24 * GiB) == MtlMemoryTier::High);
  CHECK(autoMemoryTier(32 * GiB) == MtlMemoryTier::Ultra);
  CHECK(autoMemoryTier(36 * GiB - 1 * MiB) == MtlMemoryTier::Ultra);   // the dev box (dxdiag 36863 MB)
  CHECK(autoMemoryTier(48 * GiB) == MtlMemoryTier::Ultra);
  CHECK(autoMemoryTier(64 * GiB) == MtlMemoryTier::Unlimited);
  CHECK(autoMemoryTier(128 * GiB) == MtlMemoryTier::Unlimited);

  // Names.
  MtlMemoryTier t = MtlMemoryTier::Unlimited; bool isAuto = false;
  CHECK(parseMemoryTier("auto", t, isAuto) && isAuto);
  CHECK(parseMemoryTier("", t, isAuto) && isAuto);
  CHECK(parseMemoryTier("Medium", t, isAuto) && !isAuto && t == MtlMemoryTier::Medium);
  CHECK(parseMemoryTier("UNLIMITED", t, isAuto) && !isAuto && t == MtlMemoryTier::Unlimited);
  CHECK(!parseMemoryTier("huge", t, isAuto));
  for (MtlMemoryTier x : { MtlMemoryTier::Low, MtlMemoryTier::Medium, MtlMemoryTier::High,
                           MtlMemoryTier::Ultra, MtlMemoryTier::Unlimited }) {
    CHECK(parseMemoryTier(memoryTierName(x), t, isAuto) && !isAuto && t == x);
    CHECK(memoryProfileForTier(x).tier == x);
  }

  // `unlimited` must be the pre-profile driver: 4096 MB d3d9 cap, no budget,
  // legacy pools (no staging pool), WRITEONLY buffers not direct, 90 MiB
  // staging in flight (= the old 30 MiB-per-submission x3), 4 recycled
  // backings per buffer, no global cap, keep all ring chunks, 32 allocators.
  MtlMemoryProfile u = memoryProfileForTier(MtlMemoryTier::Unlimited);
  CHECK(u.reportedVramMB == 4096u && u.budget == 0u);
  CHECK(u.legacy && !u.writeOnlyDirect && u.stagingBlocks == 0u);
  CHECK(!u.evictManagedTex && !u.evictBufferStaging && !u.residencyLru);
  CHECK(u.stagingInFlightMB == 90u && u.recycledPerBuffer == 4u && u.recycledCapBytes == 0u);
  CHECK(u.ringKeep == 0u && u.allocatorCap == 32u);
  CHECK(u.statsInterval == 0u && !u.labels);

  // `ultra` keeps unlimited's caps for now but uses the new mechanisms.
  MtlMemoryProfile ul = memoryProfileForTier(MtlMemoryTier::Ultra);
  CHECK(!ul.legacy && ul.writeOnlyDirect && ul.stagingBlocks == 8u);
  CHECK(ul.reportedVramMB == u.reportedVramMB && ul.stagingInFlightMB == u.stagingInFlightMB);
  CHECK(ul.recycledPerBuffer == u.recycledPerBuffer);
  CHECK(ul.recycledCapBytes == 0u && ul.ringKeep == 0u && ul.allocatorCap == u.allocatorCap);
  CHECK(ul.budget == 16 * GiB);

  for (MtlMemoryTier x : { MtlMemoryTier::Low, MtlMemoryTier::Medium, MtlMemoryTier::High })
    CHECK(!memoryProfileForTier(x).legacy && memoryProfileForTier(x).writeOnlyDirect
       && memoryProfileForTier(x).stagingBlocks > 0u);

  // Switch parsing (D9MT_MEMSTATS, D9MT_LABELS, D9MT_WRITEONLY_DIRECT).
  CHECK(parseSwitch("1") == 1 && parseSwitch("True") == 1 && parseSwitch("yes") == 1 && parseSwitch("ON") == 1);
  CHECK(parseSwitch("0") == 0 && parseSwitch("false") == 0 && parseSwitch("No") == 0 && parseSwitch("off") == 0);
  CHECK(parseSwitch("") == -1 && parseSwitch("256") == -1 && parseSwitch("maybe") == -1);

  // Smaller tiers never exceed the larger ones.
  const MtlMemoryTier order[] = { MtlMemoryTier::Low, MtlMemoryTier::Medium, MtlMemoryTier::High, MtlMemoryTier::Ultra };
  for (int i = 0; i + 1 < 4; i++) {
    MtlMemoryProfile a = memoryProfileForTier(order[i]), b = memoryProfileForTier(order[i + 1]);
    CHECK(a.budget < b.budget);
    CHECK(a.reportedVramMB <= b.reportedVramMB);
    CHECK(a.stagingBlocks <= b.stagingBlocks && a.stagingInFlightMB <= b.stagingInFlightMB);
    CHECK(a.recycledPerBuffer <= b.recycledPerBuffer && a.allocatorCap <= b.allocatorCap);
    CHECK(a.stagingInFlightMB >= 12u);  // >= 3 x 4 MiB blocks per submission window
  }
  CHECK(memoryProfileForTier(MtlMemoryTier::Low).reportedVramMB == 2048u);
  CHECK(memoryProfileForTier(MtlMemoryTier::Medium).reportedVramMB == 3072u);
  CHECK(memoryProfileForTier(MtlMemoryTier::High).reportedVramMB == 4096u);

  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
