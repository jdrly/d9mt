// Native unit test for FencedPool and the D3D9 staging-block reuse rule
// (D3D9DeviceEx::AllocStagingBuffer): a full block retired at staging total T
// may be reused once the staging fence (snapshot of the total at a flush,
// signalled when that submission completes) reaches T.
//   clang++ -std=c++17 -I driver/util test/native/fenced_pool_test.cpp -o /tmp/fpt && /tmp/fpt
#include "util_fenced_pool.h"

#include <cstdio>
#include <deque>
#include <random>
#include <vector>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void basics() {
  FencedPool<int> pool(2);
  int out = -1;
  CHECK(!pool.take(100, out));
  CHECK(pool.retire(1, 10));
  CHECK(pool.retire(2, 20));
  CHECK(!pool.retire(3, 30));           // full: caller drops it
  CHECK(pool.size() == 2);
  CHECK(!pool.take(9, out));            // fence not there yet
  CHECK(pool.take(10, out) && out == 1); // FIFO, ready exactly at readyAt
  CHECK(!pool.take(19, out));
  CHECK(pool.take(25, out) && out == 2);
  CHECK(pool.size() == 0);
  FencedPool<int> none(0);
  CHECK(!none.retire(1, 0));            // capacity 0 = no pooling
  pool.retire(5, 1); pool.retire(6, 2);
  pool.setCapacity(1);
  CHECK(pool.size() == 1 && pool.take(1, out) && out == 5);
}

// Event-level model of the frontend staging allocator + CS/GPU submissions.
static void simulate(uint32_t seed, size_t capacity) {
  constexpr uint64_t BlockSize = 4u << 20;
  std::mt19937 rng(seed);

  struct Block { uint64_t id, size, offset = 0, used = 0; };
  struct Slice { uint64_t block; uint64_t submission; };

  FencedPool<uint64_t> pool(capacity);
  std::vector<Block> blocks;
  std::vector<Slice> slices;          // every slice ever handed out
  std::vector<bool> submissionDone;   // index = submission number
  std::deque<std::pair<uint64_t, uint64_t>> inFlight; // (submission, staging snapshot)

  uint64_t base = 0, discard = 0, fence = 0, openSubmission = 0;
  uint64_t created = 0, reused = 0;
  submissionDone.push_back(false);
  blocks.push_back({ 0, BlockSize });
  uint64_t cur = 0;

  auto stats = [&] { return base + blocks[cur].used + discard; };

  for (int step = 0; step < 200000; step++) {
    uint32_t action = rng() % 100;
    if (action < 70) {
      // AllocStagingBuffer(size), then the copy is emitted into the open submission.
      uint64_t size = 1 + rng() % (rng() % 8 ? (256u << 10) : (5u << 20));
      Block* b = &blocks[cur];
      uint64_t off = (b->offset + 255u) & ~uint64_t(255u);
      if (off + size > b->size) {
        const uint64_t retireAt = stats();
        base += b->used;
        if (b->size == BlockSize)
          pool.retire(uint64_t(cur), retireAt);
        uint64_t id;
        if (size <= BlockSize && pool.take(fence, id)) {
          // The property under test: nothing in flight still reads this block.
          for (const Slice& s : slices)
            if (s.block == id)
              CHECK(submissionDone[s.submission]);
          blocks[id].offset = blocks[id].used = 0;
          cur = id; reused++;
        } else {
          blocks.push_back({ blocks.size(), std::max(size, BlockSize) });
          cur = blocks.size() - 1; created++;
        }
        b = &blocks[cur];
        off = 0;
      }
      b->offset = off + size;
      b->used += size;
      slices.push_back({ uint64_t(cur), openSubmission });
    } else if (action < 75) {
      discard += 1 + rng() % (64u << 10);  // DISCARD renames count toward the total too
    } else if (action < 85) {
      // ExecuteFlush: the fence will be signalled with this snapshot.
      inFlight.push_back({ openSubmission, stats() });
      submissionDone.push_back(false);
      openSubmission++;
    } else if (!inFlight.empty()) {
      // GPU completes the oldest submission (in order).
      submissionDone[inFlight.front().first] = true;
      fence = std::max(fence, inFlight.front().second);
      inFlight.pop_front();
    }
    if (slices.size() > 4096)               // keep the check affordable
      slices.erase(slices.begin(), slices.begin() + 2048);
  }
  CHECK(reused > 0);
  if (capacity >= 8)
    CHECK(reused > created);                // the pool replaces most allocations (no throttle modelled)
  std::printf("  seed %u cap %zu: %llu blocks created, %llu reused\n", seed, capacity,
    (unsigned long long)created, (unsigned long long)reused);
}

int main() {
  basics();
  for (uint32_t seed : { 1u, 2u, 3u, 42u })
    for (size_t cap : { size_t(2), size_t(8) })
      simulate(seed, cap);
  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
  return g_fail ? 1 : 0;
}
