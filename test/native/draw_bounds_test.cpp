// Native unit test for the draw bounds (#61, no Wine, no Metal), under ASan/UBSan:
//   clang++ -std=c++17 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I driver/backend test/native/draw_bounds_test.cpp -o /tmp/dbt && /tmp/dbt
// Besides the cases, a grid of stream shapes checks the invariant the clamp exists for: every
// element a clamped draw fetches lies inside the stream, and a draw that fits is never cut.
#include "mtl_draw_bounds.h"

#include <cstdio>

using namespace dxvk::backend;
using namespace dxvk::backend::drawbounds;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static Stream perVertex(uint64_t length, uint32_t stride, uint32_t size) {
  return { length, stride, size, uint8_t(kPsoStepPerVertex) };
}

static Stream perInstance(uint64_t length, uint32_t stride, uint32_t size, uint32_t divisor) {
  return { length, stride, size, psoStepFunction(stride, true, divisor) };
}

static void testCases() {
  Limits l;
  CHECK(l.vertices == Unbounded && l.instances == Unbounded);

  // 768 vertices of 24 bytes: 768 fit, a draw from 700 keeps 68
  add(l, perVertex(768u * 24u, 24u, 24u));
  CHECK(l.vertices == 768u);
  CHECK(clamp(0u, 768u, l.vertices) == 768u);
  CHECK(clamp(700u, 256u, l.vertices) == 68u);
  CHECK(clamp(768u, 3u, l.vertices) == 0u);
  CHECK(clamp(0xffffffffu, 3u, l.vertices) == 0u);

  // a second, shorter stream narrows it; the declaration reads 20 of a 24-byte stride
  add(l, perVertex(100u * 24u + 20u, 24u, 20u));
  CHECK(l.vertices == 101u);

  // a cut-off last vertex doesn't count; a slice shorter than one vertex allows none
  Limits c;
  add(c, perVertex(10u * 16u + 15u, 16u, 16u));
  CHECK(c.vertices == 10u);
  add(c, perVertex(15u, 16u, 16u));
  CHECK(c.vertices == 0u);

  // an attribute past the stride (vertex size 32 at stride 16): the last vertex reads 16 past
  Limits o;
  add(o, perVertex(10u * 16u, 16u, 32u));
  CHECK(o.vertices == 9u);

  // constant streams (stride 0, unbound, the null stream) limit nothing
  Limits k;
  add(k, { 4u, 0u, 16u, uint8_t(kPsoStepConstant) });
  add(k, { 0u, 0u, 16u, uint8_t(kPsoStepConstant) });
  add(k, perInstance(0u, 16u, 16u, 0u));   // divisor 0: constant
  CHECK(k.vertices == Unbounded && k.instances == Unbounded);

  // per instance: 2 elements at divisor 2 serve 4 instances; at divisor 1, 2
  Limits i;
  add(i, perInstance(2u * 20u, 20u, 20u, 2u));
  CHECK(i.instances == 4u && i.vertices == Unbounded);
  add(i, perInstance(2u * 20u, 20u, 20u, 1u));
  CHECK(i.instances == 2u);
  CHECK(clamp(0u, 4u, i.instances) == 2u);

  // large slices saturate instead of wrapping
  Limits big;
  add(big, perVertex(uint64_t(1) << 40, 4u, 4u));
  CHECK(big.vertices == Unbounded);
  add(big, perInstance(uint64_t(1) << 40, 4u, 4u, 254u));
  CHECK(big.instances == Unbounded);
}

// Every shape: a clamped draw reads inside, and a draw that fits keeps its count.
static void testGrid() {
  uint64_t n = 0u;
  for (uint32_t stride = 1u; stride <= 40u; stride++) {
    for (uint32_t size = 1u; size <= 48u; size++) {
      for (uint64_t length = 0u; length <= 600u; length += 7u) {
        Limits l;
        add(l, perVertex(length, stride, size));
        for (uint32_t first = 0u; first <= 60u; first += 3u) {
          for (uint32_t count = 0u; count <= 70u; count += 5u) {
            const uint32_t kept = clamp(first, count, l.vertices);
            CHECK(kept <= count);
            if (kept)   // the last fetched vertex ends inside
              CHECK(uint64_t(first + kept - 1u) * stride + size <= length);
            const bool fits = !count || uint64_t(first + count - 1u) * stride + size <= length;
            if (fits)
              CHECK(kept == count);
            n++;
          }
        }
      }
    }
  }
  std::printf("draw_bounds grid: %llu draws\n", (unsigned long long)n);
}

int main() {
  testCases();
  testGrid();
  std::printf("draw_bounds_test: %s\n", g_fail ? "FAILED" : "OK");
  return g_fail ? 1 : 0;
}
