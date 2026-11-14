// Native unit test for the indexed fan expansion (#187, no Wine, no Metal), under ASan/UBSan:
//   clang++ -std=c++17 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I driver/d3d9 test/native/fan_indices_test.cpp -o /tmp/fit && /tmp/fit
// Each source is a heap block of exactly the indices the clamp allows, so a read past them is an
// ASan error, not a silent pass.
#include "d3d9_fan_indices.h"

#include <cstdio>
#include <vector>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

template <typename T>
static std::vector<T> expandFan(const std::vector<T>& src, uint32_t count) {
  std::vector<T> dst(3u * count);
  fan::expand(dst.data(), src.data(), count, uint32_t(sizeof(T)));
  return dst;
}

static void testTriangles() {
  CHECK(fan::triangles(4u, 6u) == 4u);
  CHECK(fan::triangles(4u, 100u) == 4u);
  CHECK(fan::triangles(4u, 5u) == 3u);    // the buffer ends early: what fits
  CHECK(fan::triangles(4u, 3u) == 1u);
  CHECK(fan::triangles(4u, 2u) == 0u);    // fewer than 3 indices: nothing
  CHECK(fan::triangles(4u, 0u) == 0u);
  CHECK(fan::triangles(0u, 6u) == 0u);
  CHECK(fan::triangles(0xffffffffu, 0x100000000ull) == 0xfffffffeu);
  CHECK(fan::triangles(0xffffffffu, ~0ull) == 0xffffffffu);
}

static void testExpand() {
  // a 16-bit fan with a hub that isn't 0 and indices out of order
  const std::vector<uint16_t> s16 = { 7, 3, 9, 1, 0xfffe, 4 };
  const std::vector<uint16_t> w16 = { 7, 3, 9,  7, 9, 1,  7, 1, 0xfffe,  7, 0xfffe, 4 };
  CHECK(expandFan(s16, 4u) == w16);

  // 32-bit indices keep their high bits
  const std::vector<uint32_t> s32 = { 0x10000u, 0x20001u, 5u, 0xffffffffu };
  const std::vector<uint32_t> w32 = { 0x10000u, 0x20001u, 5u,  0x10000u, 5u, 0xffffffffu };
  CHECK(expandFan(s32, 2u) == w32);

  // a single triangle is its three indices
  const std::vector<uint16_t> one = { 2, 1, 0 };
  CHECK(expandFan(one, 1u) == one);
}

// Every buffer length the clamp sees, from an offset: the expansion reads only the indices that
// remain, writes exactly 3 per triangle, and each triangle t is (hub, t+1, t+2).
static void testGrid() {
  for (uint32_t size : { 2u, 4u }) {
    for (uint64_t available = 0u; available < 40u; available++) {
      for (uint32_t primitives : { 0u, 1u, 2u, 7u, 37u, 38u, 100u }) {
        const uint32_t count = fan::triangles(primitives, available);
        CHECK(count <= primitives);
        CHECK(count == 0u || count + 2u <= available);
        if (available >= 3u && primitives + 2u <= available)
          CHECK(count == primitives);   // a fan that fits is never cut
        if (!count)
          continue;
        std::vector<uint8_t> src(size_t(count + 2u) * size);   // exactly what it may read
        for (size_t i = 0u; i < src.size(); i++)
          src[i] = uint8_t(i * 7u + 1u);
        std::vector<uint8_t> dst(size_t(count) * 3u * size);
        fan::expand(dst.data(), src.data(), count, size);
        for (uint32_t t = 0u; t < count; t++) {
          CHECK(!std::memcmp(&dst[(3u * t + 0u) * size], &src[0], size));
          CHECK(!std::memcmp(&dst[(3u * t + 1u) * size], &src[(t + 1u) * size], size));
          CHECK(!std::memcmp(&dst[(3u * t + 2u) * size], &src[(t + 2u) * size], size));
        }
      }
    }
  }
}

int main() {
  testTriangles();
  testExpand();
  testGrid();
  std::printf("fan_indices_test: %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
