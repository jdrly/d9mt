// Unit test for one stream's clamp and copy in D3D9DeviceEx::UploadPerDrawData, under
// AddressSanitizer and UndefinedBehaviorSanitizer. It compiles the driver's own statements:
// scripts/test-upload-copy.sh cuts them out of d3d9_device.cpp (test/native/upload_copy_extract.py)
// into the three .inc files included below, and runs them against a fake device:
//  - the vertex buffer's CPU copy and the upload slice are heap allocations of exactly their size,
//    so ASan reports any byte read or written past them;
//  - the copy's length is what the case asks for, worked out from the case alone (of each
//    requested vertex its first min(stride, vertex size) bytes, as far as they lie inside the
//    buffer), and the copied bytes are the source's;
//  - SWTOR's hint (#56): first vertex 600, 256 vertices of 24 bytes in a 768-vertex buffer copies
//    the 168 vertices left from vertex 600, not 18432 bytes from it;
//  - zero strides, a first vertex at and past the end, packed and strided partial last vertices,
//    and counts whose byte size wraps 32 bits.
//
//   scripts/test-upload-copy.sh [d3d9_device.cpp]
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#define likely(x)   __builtin_expect(bool(x), 1)
#define unlikely(x) __builtin_expect(bool(x), 0)

typedef unsigned int UINT;
typedef int INT;
constexpr uint32_t D3DSTREAMSOURCE_INSTANCEDATA = 2u << 30;

static int g_fail = 0;
static long g_cases = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// What the fragments use of the device, one stream (slot 0).
struct FakeDevice {
  struct Slice { void* mapPtr; };
  struct BufferDesc { uint32_t Size; };
  struct Buffer {
    Slice mapped;
    BufferDesc desc;
    const BufferDesc* Desc() const { return &desc; }
    const Slice* GetMappedSlice() const { return &mapped; }
  };
  struct Decl {
    uint32_t size;
    uint32_t GetSize(uint32_t) const { return size; }
  };
  struct Stream { Buffer* vertexBuffer; uint32_t offset; uint32_t stride; };
  struct {
    const Decl* vertexDecl;
    Stream vertexBuffers[1];
    uint32_t streamFreq[1];
  } m_state = { };
  uint32_t instances = 0;

  Buffer* GetCommonBuffer(Buffer* b) const { return b; }
  uint32_t GetInstanceCount() const { return instances; }

#include "upload_vbocopy.inc"

  void clamp(uint32_t i, UINT FirstVertexIndex, UINT NumVertices, INT BaseVertexIndex,
             std::array<VBOCopy, 1>& vboCopies, uint32_t& totalUpBufferSize) {
    auto* vbo = GetCommonBuffer(m_state.vertexBuffers[i].vertexBuffer);
#include "upload_clamp.inc"
  }

  void copyOne(uint32_t i, const VBOCopy& copy, Slice upSlice) {
#include "upload_copy.inc"
  }
};

struct Case {
  uint32_t size, vboOffset, first; int32_t base; uint32_t count, stride, vertexSize; bool instanced;
};

// The driver's sequence for one stream: clamp, then copy into an upload slice of the total size.
// Returns the clamp's result for the explicit cases' expectations.
static FakeDevice::VBOCopy run(const Case& t) {
  g_cases++;
  FakeDevice dev;
  FakeDevice::Buffer vb = { { nullptr }, { t.size } };
  FakeDevice::Decl decl = { t.vertexSize };
  dev.m_state.vertexDecl = &decl;
  dev.m_state.vertexBuffers[0] = { &vb, t.vboOffset, t.stride };
  dev.m_state.streamFreq[0] = t.instanced ? D3DSTREAMSOURCE_INSTANCEDATA | 1u : 1u;
  dev.instances = t.count;

  std::array<FakeDevice::VBOCopy, 1> copies = { };
  uint32_t total = 0;
  dev.clamp(0, t.first, t.instanced ? 1u : t.count, t.base, copies, total);
  const FakeDevice::VBOCopy c = copies[0];
  CHECK(total == c.copyBufferLength);
  // the length from the case alone: the first vertex at the stream offset plus (first + base)
  // vertices (D3D9's 32-bit offset), then of each requested vertex its first min(stride, vertex
  // size) bytes, as far as they lie inside the buffer
  const uint32_t start = t.vboOffset + (t.first + uint32_t(t.base)) * t.stride;
  const uint32_t part = std::min(t.stride, t.vertexSize);
  uint64_t expected = 0;
  if (start < t.size && part != 0u)
    for (uint64_t j = 0; j < t.count && start + j * t.stride < t.size; j++)
      expected += std::min<uint64_t>(part, t.size - (start + j * t.stride));
  CHECK(c.srcOffset == start);
  CHECK(total == expected);
  if (total == 0u)
    return c;   // the driver allocates and copies nothing

  // the upload slice first: a read past the CPU copy then runs into ASan's redzone after it, not
  // into the slice (ASan would report that as an overlap instead of the overflow)
  std::unique_ptr<uint8_t[]> dst(new uint8_t[total]);
  std::unique_ptr<uint8_t[]> src(new uint8_t[t.size]);
  for (uint32_t i = 0; i < t.size; i++)
    src[i] = uint8_t(i * 7u + 3u);
  vb.mapped.mapPtr = src.get();
  dev.copyOne(0, c, { dst.get() });   // ASan: past src or dst is a report

  // the copy covers the vertices it names and ends inside the buffer
  const uint32_t size = c.copyElementSize, stride = c.copyElementStride;
  const uint64_t whole = uint64_t(c.copyElementCount) * size;
  const uint64_t tail = c.copyBufferLength - whole;
  CHECK(c.copyBufferLength >= whole && tail <= size);
  const uint64_t end = uint64_t(c.srcOffset) + uint64_t(c.copyElementCount) * stride + tail;
  CHECK(end <= t.size);
  if (end > t.size)
    return c;
  for (uint32_t j = 0; j < c.copyElementCount; j++)
    CHECK(std::memcmp(dst.get() + uint64_t(j) * size, src.get() + c.srcOffset + uint64_t(j) * stride, size) == 0);
  if (tail)
    CHECK(std::memcmp(dst.get() + whole, src.get() + c.srcOffset + uint64_t(c.copyElementCount) * stride, tail) == 0);

  // nothing that fits is left out: all of the range, or every whole vertex up to the end
  if (uint64_t(c.srcOffset) + uint64_t(t.count) * stride <= t.size)
    CHECK(c.copyElementCount == t.count && tail == 0u);
  else {
    const uint64_t left = t.size - (uint64_t(c.srcOffset) + uint64_t(c.copyElementCount) * stride);
    CHECK(left < stride && tail == std::min<uint64_t>(size, left));
  }
  return c;
}

int main() {
  // SWTOR's pattern (#56), by first vertex, by base vertex and by instance count
  for (const Case& t : { Case{ 768u * 24u, 0u, 600u, 0, 256u, 24u, 24u, false },
                         Case{ 768u * 24u, 0u, 610u, -10, 256u, 24u, 24u, false },
                         Case{ 768u * 24u, 0u, 600u, 0, 256u, 24u, 24u, true } }) {
    const auto c = run(t);
    CHECK(c.copyElementCount == 168u);
    CHECK(c.copyBufferLength == 168u * 24u);
    CHECK(c.srcOffset + c.copyBufferLength == 768u * 24u);
  }
  // a first vertex exactly at the end, and one past it: nothing copied
  CHECK(run({ 4096u, 0u, 256u, 0, 4u, 16u, 16u, false }).copyBufferLength == 0u);
  CHECK(run({ 4096u, 0u, 257u, 0, 4u, 16u, 16u, false }).copyBufferLength == 0u);
  CHECK(run({ 4096u, 4100u, 0u, 0, 4u, 16u, 16u, false }).copyBufferLength == 0u);
  // a zero stride: nothing to copy, and no division by it
  CHECK(run({ 4096u, 16u, 5u, 0, 256u, 0u, 16u, false }).copyBufferLength == 0u);
  // partial last vertices: packed (the whole rest) and strided (as much of the vertex as is left)
  CHECK(run({ 100u, 0u, 2u, 0, 10u, 24u, 24u, false }).copyBufferLength == 52u);
  CHECK(run({ 100u, 0u, 2u, 0, 10u, 24u, 16u, false }).copyBufferLength == 2u * 16u + 4u);
  // counts whose byte size wraps 32 bits (0x0AAAAAAB x 24 = 8 mod 2^32): still clamped
  CHECK(run({ 4096u, 0u, 0u, 0, 0x0AAAAAABu, 24u, 24u, false }).copyBufferLength == 4096u);
  CHECK(run({ 4096u, 0u, 0u, 0, 0x0AAAAAABu, 24u, 12u, false }).copyElementCount == 4096u / 24u);

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
              run({ size, off, first, 0, count, stride, vs, false });

  std::printf("%s upload_copy_test (%ld cases, %d failures)\n", g_fail ? "FAIL" : "PASS", g_cases, g_fail);
  return g_fail ? 1 : 0;
}
