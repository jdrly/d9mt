// Unit test for psoBufferLayouts() (driver/backend/mtl_pso_key.h): the Metal vertex buffer layouts
// RenderPsoCache::build() hands the bridge.
//   - only the key's layouts are set; every other index below the highest is Metal's default
//     (stride 0, per vertex, step rate 1), which the bridge leaves out of the descriptor, never
//     a zeroed layout (stride 0, constant), which it would pass to Metal;
//   - SWTOR's first fixed-function draw: stream 0 at buffer 3, the null stream (constant) at 19;
//   - per vertex, constant and per instance (step rate = the value - 1) as before;
//   - an index past 30 is ignored, and the count is the highest set index + 1.
//   clang++ -std=c++17 -I driver/backend test/native/pso_buffer_layouts_test.cpp -o t && ./t
#include "mtl_pso_key.h"

#include <cstdio>

using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static bool isDefault(const PsoBufferLayout& l) {
  return !l.set && l.stride == 0u && l.stepFunction == kPsoStepPerVertex && l.stepRate == 1u;
}

int main() {
  // SWTOR's first FF draw: stream 0 (stride 32) at buffer 3, the null stream (constant, the
  // declaration's stride 16) at 19
  {
    RenderPsoKey key = {};
    key.layouts[0].pad = 3u;  key.layouts[0].stride = 32u; key.layouts[0].stepFunction = kPsoStepPerVertex;
    key.layouts[1].pad = 19u; key.layouts[1].stride = 16u; key.layouts[1].stepFunction = kPsoStepConstant;
    key.layoutCount = 2u;
    PsoBufferLayout out[kPsoMaxBufferLayouts];
    const uint32_t n = psoBufferLayouts(key, out);
    CHECK(n == 20u);
    CHECK(out[3].set && out[3].stride == 32u && out[3].stepFunction == kPsoStepPerVertex && out[3].stepRate == 1u);
    CHECK(out[19].set && out[19].stride == 16u && out[19].stepFunction == kPsoStepConstant && out[19].stepRate == 0u);
    for (uint32_t i = 0u; i < kPsoMaxBufferLayouts; i++) {
      if (i == 3u || i == 19u)
        continue;
      CHECK(isDefault(out[i]));
      // never what the bridge would send to Metal: stride 0 with a step other than per vertex
      CHECK(!(out[i].stride == 0u && out[i].stepFunction != kPsoStepPerVertex));
    }
  }

  // Per instance: step function byte 2..255 -> step rate value - 1
  {
    RenderPsoKey key = {};
    key.layouts[0].pad = 4u; key.layouts[0].stride = 12u; key.layouts[0].stepFunction = uint8_t(psoStepFunction(12u, true, 3u));
    key.layoutCount = 1u;
    PsoBufferLayout out[kPsoMaxBufferLayouts];
    CHECK(psoBufferLayouts(key, out) == 5u);
    CHECK(out[4].set && out[4].stepFunction == 2u && out[4].stepRate == 3u && out[4].stride == 12u);
    for (uint32_t i = 0u; i < 4u; i++)
      CHECK(isDefault(out[i]));
  }

  // An index past 30 is ignored; no layouts gives 0
  {
    RenderPsoKey key = {};
    key.layouts[0].pad = 31u; key.layouts[0].stride = 8u; key.layouts[0].stepFunction = kPsoStepPerVertex;
    key.layoutCount = 1u;
    PsoBufferLayout out[kPsoMaxBufferLayouts];
    CHECK(psoBufferLayouts(key, out) == 0u);
    for (const PsoBufferLayout& l : out)
      CHECK(isDefault(l));
  }

  // The output is fully reset: a reused array keeps nothing of the previous key
  {
    PsoBufferLayout out[kPsoMaxBufferLayouts];
    for (PsoBufferLayout& l : out) { l.set = true; l.stride = 0u; l.stepFunction = kPsoStepConstant; l.stepRate = 0u; }
    RenderPsoKey key = {};
    CHECK(psoBufferLayouts(key, out) == 0u);
    for (const PsoBufferLayout& l : out)
      CHECK(isDefault(l));
  }

  std::printf("pso_buffer_layouts_test: %s (%d failures)\n", g_fail ? "FAIL" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
