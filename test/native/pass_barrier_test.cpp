// Native unit test of the producer barrier's stage masks (#64): MtlContextImpl::emitProducerBarrier
// (driver/backend/mtl_context_transfer.cpp), compiled from the driver's own source text
// (test/native/pass_barrier_extract.py cuts it out by its anchors) against a recording
// MTL4CommandEncoder_barrier. For every mask its callers pass, and a few more:
//  - an afterStages with Fragment also waits on Vertex; any other afterStages is passed as is;
//  - beforeStages is passed as is (the narrowed pass end keeps Fragment|Blit|Dispatch, so the
//    next pass's vertex work still overlaps);
//  - visibility is Device; a null encoder encodes nothing;
//  - the vertex epoch advances once, only for an encoder whose caller's afterStages has Fragment
//    and whose beforeStages has Vertex (the full pass end and the vertex fences).
// This checks the commands d9mt encodes, not GPU ordering. The hazard itself hasn't been observed:
// by Apple's description its GPUs run a pass's vertex work before its fragment work, and it was
// tested on one M5 Max only (#64).
//   scripts/test-pass-barrier.sh   (scripts/test-pass-barrier.sh --witness <base file>: also the
//                                   base and the mutants, which must fail)
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "../../bridge/winemetal.h"

static int g_fail = 0;

static uint64_t g_calls = 0u, g_encoder = 0u, g_after = 0u, g_before = 0u, g_visibility = 0u;

extern "C" void MTL4CommandEncoder_barrier(obj_handle_t encoder, uint64_t after, uint64_t before, uint64_t visibility) {
  g_calls++;
  g_encoder = encoder;
  g_after = after;
  g_before = before;
  g_visibility = visibility;
}

namespace dxvk::backend {

  struct MtlContextImpl {
    uint64_t m_vtxEpoch = 19u;
    void emitProducerBarrier(obj_handle_t encoder, uint64_t afterStages,
                             uint64_t beforeStages = uint64_t(WMTStageAll));
  };

#include "emit_producer_barrier.inc"

}

using dxvk::backend::MtlContextImpl;

static constexpr uint64_t V = uint64_t(WMTStageVertex), F = uint64_t(WMTStageFragment);
static constexpr uint64_t BD = uint64_t(WMTStageBlit) | uint64_t(WMTStageDispatch);
static constexpr uint64_t All = uint64_t(WMTStageAll);

static void check(const char* what, obj_handle_t enc, uint64_t after, uint64_t before) {
  MtlContextImpl c;
  g_calls = g_encoder = g_after = g_before = g_visibility = 0u;
  c.emitProducerBarrier(enc, after, before);

  const uint64_t wantAfter = (after & F) ? after | V : after;
  const uint64_t wantEpoch = 19u + ((enc && (after & F) && (before & V)) ? 1u : 0u);
  bool ok = g_calls == (enc ? 1u : 0u) && c.m_vtxEpoch == wantEpoch;
  if (enc) {
    ok = ok && g_encoder == enc && g_after == wantAfter && g_before == before
            && g_visibility == uint64_t(WMTVisibilityOptionDevice);
  }
  if (!ok) {
    g_fail++;
    std::printf("FAIL %s: enc=%llx after=%llx before=%llx -> calls=%llu after=%llx (want %llx) before=%llx "
                "visibility=%llu epoch+%llu (want +%llu)\n", what,
      (unsigned long long)enc, (unsigned long long)after, (unsigned long long)before,
      (unsigned long long)g_calls, (unsigned long long)g_after, (unsigned long long)wantAfter,
      (unsigned long long)g_before, (unsigned long long)g_visibility,
      (unsigned long long)(c.m_vtxEpoch - 19u), (unsigned long long)(wantEpoch - 19u));
  }
}

int main() {
  static_assert(V == 1u && F == 2u, "MTLStageVertex, MTLStageFragment");

  for (obj_handle_t enc : { obj_handle_t(0x7u), obj_handle_t(0u) }) {
    // What the callers pass (mtl_context_record.cpp, mtl_context_transfer.cpp).
    check("full pass end",           enc, F,  All);
    check("narrowed pass end",       enc, F,  F | BD);
    check("vertex fence",            enc, F,  V);
    check("vertex fence (transfer)", enc, F,  All);
    check("blit/compute end",        enc, BD, All);
    // Other masks: nothing else changes.
    check("vertex|fragment",         enc, V | F, All);
    check("vertex only",             enc, V,  All);
    check("no stage",                enc, 0u, All);
    check("blit before vertex",      enc, BD, V);
    check("fragment before none",    enc, F,  0u);
  }

  std::printf("%s\n", g_fail ? "FAIL" : "ok");
  return g_fail ? 1 : 0;
}
