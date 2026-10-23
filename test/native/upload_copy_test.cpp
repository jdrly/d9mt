// Unit test for one stream's copy in UploadPerDrawData (driver/d3d9/d3d9_upload_copy.h, the code
// the driver runs), for AddressSanitizer and UndefinedBehaviorSanitizer:
//  - every copy stays inside the vertex buffer's CPU copy and the upload slice: both are heap
//    allocations of exactly their size, so ASan reports any byte read or written past them;
//  - the copied bytes are the source's (whole vertices, the first copyElementSize bytes of each, a
//    vertex the buffer's end cuts through as far as it goes);
//  - SWTOR's hint (#56): first vertex 600, 256 vertices of 24 bytes in a 768-vertex buffer copies
//    the 168 vertices left from vertex 600, not 18432 bytes from it;
//  - a count whose byte size wraps 32 bits is still clamped to the buffer.
// Native (no Windows headers):
//   clang++ -std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//     -I driver/d3d9 test/native/upload_copy_test.cpp -o upload_copy_test && ./upload_copy_test
#include "d3d9_upload_copy.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace dxvk;

static int g_fail = 0;
static long g_cases = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// The driver's sequence for one stream: clamp, then copy if anything is left.
static void run(uint32_t size, uint32_t vboOffset, uint32_t first, uint32_t count, uint32_t srcStride, uint32_t vertexSize) {
  g_cases++;
  const uint32_t dstStride = std::min(srcStride, vertexSize);
  const uint64_t srcOffset64 = uint64_t(vboOffset) + uint64_t(first) * srcStride;
  if (srcOffset64 > 0xFFFFFFFFull)
    return;   // the driver's 32-bit srcOffset: not this function's input range
  const uint32_t srcOffset = uint32_t(srcOffset64);

  std::unique_ptr<uint8_t[]> src(new uint8_t[size ? size : 1]);
  for (uint32_t i = 0; i < size; i++)
    src[i] = uint8_t(i * 7u + 3u);

  D3D9VboCopy copy = { };
  d3d9ClampVboCopy(copy, srcOffset, count, srcStride, dstStride, size);
  if (copy.copyBufferLength == 0u)
    return;

  // what the copy reads ends inside the buffer
  const uint64_t whole = uint64_t(copy.copyElementCount) * dstStride;
  const uint64_t readEnd = copy.copyElementStride == copy.copyElementSize
    ? uint64_t(srcOffset) + copy.copyBufferLength
    : uint64_t(srcOffset) + (copy.copyElementCount ? uint64_t(copy.copyElementCount - 1u) * srcStride + dstStride : 0u);
  CHECK(readEnd <= size);
  CHECK(uint64_t(srcOffset) + uint64_t(copy.copyElementCount) * srcStride + (copy.copyBufferLength - whole) <= size);

  std::unique_ptr<uint8_t[]> dst(new uint8_t[copy.copyBufferLength]);
  d3d9CopyVbo(dst.get(), src.get() + srcOffset, copy);   // ASan: past src or dst is a report

  for (uint32_t j = 0; j < copy.copyElementCount; j++)
    CHECK(std::memcmp(dst.get() + j * dstStride, src.get() + srcOffset + j * srcStride, dstStride) == 0);
  if (copy.copyBufferLength > whole)
    CHECK(std::memcmp(dst.get() + whole, src.get() + srcOffset + uint64_t(copy.copyElementCount) * srcStride,
                      copy.copyBufferLength - whole) == 0);
}

int main() {
  // SWTOR's pattern (#56)
  {
    D3D9VboCopy c = { };
    d3d9ClampVboCopy(c, 600u * 24u, 256u, 24u, 24u, 768u * 24u);
    CHECK(c.copyElementCount == 168u);
    CHECK(c.copyBufferLength == 168u * 24u);
    CHECK(c.srcOffset + c.copyBufferLength == 768u * 24u);
    run(768u * 24u, 0u, 600u, 256u, 24u, 24u);
  }
  // a count whose byte size wraps 32 bits: still clamped to the buffer
  {
    D3D9VboCopy c = { };
    d3d9ClampVboCopy(c, 0u, 0x0AAAAAABu, 24u, 24u, 4096u);   // 0x0AAAAAAB * 24 wraps to 8
    CHECK(c.copyBufferLength <= 4096u);
    run(4096u, 0u, 0u, 0x0AAAAAABu, 24u, 24u);
  }
  // a first vertex exactly at the end, and one past it: nothing copied
  {
    D3D9VboCopy c = { };
    d3d9ClampVboCopy(c, 4096u, 4u, 16u, 16u, 4096u);
    CHECK(c.copyBufferLength == 0u);
    d3d9ClampVboCopy(c, 4097u, 4u, 16u, 16u, 4096u);
    CHECK(c.copyBufferLength == 0u);
  }
  // the grid: buffer sizes, stream offsets, first vertices, counts, strides and vertex sizes,
  // packed (stride = vertex size) and strided copies, partial last vertices
  static const uint32_t sizes[]   = { 0u, 1u, 7u, 23u, 24u, 25u, 48u, 100u, 1000u, 18432u };
  static const uint32_t offsets[] = { 0u, 1u, 4u, 20u, 64u };
  static const uint32_t firsts[]  = { 0u, 1u, 3u, 40u, 513u, 600u, 767u, 768u, 769u, 5000u };
  static const uint32_t counts[]  = { 0u, 1u, 2u, 7u, 256u, 2000u, 0x00FFFFFFu, 0x0AAAAAABu, 0xFFFFFFFFu };
  static const uint32_t strides[] = { 0u, 1u, 4u, 12u, 16u, 20u, 24u, 28u, 32u, 100u };
  static const uint32_t vsizes[]  = { 4u, 12u, 16u, 24u, 32u };
  for (uint32_t size : sizes)
    for (uint32_t off : offsets)
      for (uint32_t first : firsts)
        for (uint32_t count : counts)
          for (uint32_t stride : strides)
            for (uint32_t vs : vsizes)
              run(size, off, first, count, stride, vs);

  std::printf("%s upload_copy_test (%ld cases, %d failures)\n", g_fail ? "FAIL" : "PASS", g_cases, g_fail);
  return g_fail ? 1 : 0;
}
