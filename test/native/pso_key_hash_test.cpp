// The PSO key's hash (docs/pso-key-hash.md §4): quality before speed.
//  1. Avalanche: random keys, every input bit flipped; every output bit must flip with
//     probability 0.5 +- 0.02, and every input bit must flip 0.5 +- 0.05 of the output bits.
//  2. Real keys, from state cache files given as arguments (each entry's state bytes, the
//     cookies from its shader SHA-1s): no full collision, and bucket counts (libstdc++'s
//     hash % bucket_count, for 13, 97, 1031 and 8191 buckets) no worse than the byte-wise FNV's
//     on the same keys or within the bound a random hash meets.
//  3. ns per hash, old and new (information only; the window measures).
//   pso_key_hash_test [state files...]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "backend/mtl_pso_key.h"
#include "mtl/mtl_state_cache_core.h"

using dxvk::backend::RenderPsoKey;
namespace sc = dxvk::statecache;

static int g_failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_failures++; } } while (0)

static_assert(sizeof(RenderPsoKey) == RenderPsoKey::StateOffset + sc::KeyBytes, "state bytes = key minus cookies");

// The hash before docs/pso-key-hash.md, for comparison.
static size_t fnvHash(const RenderPsoKey& k) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0u; i < sizeof(k); i++)
    h = (h ^ p[i]) * 0x100000001b3ull;
  return size_t(h ^ (h >> 32));
}

static RenderPsoKey randomKey(std::mt19937_64& rng) {
  RenderPsoKey k;
  uint8_t* p = reinterpret_cast<uint8_t*>(&k);
  for (size_t i = 0u; i < sizeof(k); i += 8u) {
    const uint64_t w = rng();
    std::memcpy(p + i, &w, 8u);
  }
  return k;
}

static void testAvalanche() {
  std::mt19937_64 rng(42u);
  constexpr uint32_t Keys = 4000u, Bits = sizeof(RenderPsoKey) * 8u;
  std::vector<uint64_t> outFlips(64u, 0u);
  double worstIn = 0.0;
  std::vector<uint64_t> inFlips(Bits, 0u);
  for (uint32_t n = 0u; n < Keys; n++) {
    RenderPsoKey k = randomKey(rng);
    const uint64_t h0 = uint64_t(k.hash());
    uint8_t* p = reinterpret_cast<uint8_t*>(&k);
    for (uint32_t b = 0u; b < Bits; b++) {
      p[b / 8u] ^= uint8_t(1u << (b % 8u));
      const uint64_t d = uint64_t(k.hash()) ^ h0;
      p[b / 8u] ^= uint8_t(1u << (b % 8u));
      inFlips[b] += uint64_t(__builtin_popcountll(d));
      for (uint32_t o = 0u; o < 64u; o++)
        outFlips[o] += (d >> o) & 1u;
    }
  }
  double worstOut = 0.0;
  for (uint32_t o = 0u; o < 64u; o++)
    worstOut = std::max(worstOut, std::fabs(double(outFlips[o]) / double(Keys * Bits) - 0.5));
  for (uint32_t b = 0u; b < Bits; b++)
    worstIn = std::max(worstIn, std::fabs(double(inFlips[b]) / double(Keys * 64u) - 0.5));
  std::printf("avalanche: %u keys x %u bits: worst output bit |p-0.5| = %.4f, worst input bit = %.4f\n",
    Keys, Bits, worstOut, worstIn);
  CHECK(worstOut < 0.02);
  CHECK(worstIn < 0.05);
}

static std::vector<RenderPsoKey> readKeys(const char* path) {
  std::ifstream f(path, std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::vector<RenderPsoKey> keys;
  if (data.size() < 32u)
    return keys;
  uint64_t sig;
  std::memcpy(&sig, data.data() + 24u, 8u);   // the file's own key signature (any mapping version)
  std::vector<sc::Entry> es;
  const sc::ParseResult r = sc::parse(data.data(), data.size(), sig, es);
  std::printf("%s: status=%d entries=%zu\n", path, int(r.status), es.size());
  for (const sc::Entry& e : es) {
    RenderPsoKey k;
    std::memcpy(&k.vsCookie, e.vsSha, 8u);
    std::memcpy(&k.fsCookie, e.psSha, 8u);
    std::memcpy(reinterpret_cast<uint8_t*>(&k) + RenderPsoKey::StateOffset, e.key, sc::KeyBytes);
    keys.push_back(k);
  }
  return keys;
}

static double chiSquare(const std::vector<size_t>& hashes, size_t buckets) {
  std::vector<double> c(buckets, 0.0);
  for (size_t h : hashes)
    c[h % buckets] += 1.0;
  const double e = double(hashes.size()) / double(buckets);
  double x = 0.0;
  for (double v : c)
    x += (v - e) * (v - e) / e;
  return x;
}

static void testRealKeys(const std::vector<RenderPsoKey>& all) {
  // Distinct keys only (two files may share some).
  std::vector<RenderPsoKey> keys;
  {
    std::set<std::vector<uint8_t>> seen;
    for (const RenderPsoKey& k : all) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
      if (seen.insert(std::vector<uint8_t>(p, p + sizeof(k))).second)
        keys.push_back(k);
    }
  }
  if (keys.size() < 50u) {
    std::printf("real keys: %zu distinct (too few for buckets; collisions only)\n", keys.size());
  }
  std::vector<size_t> hNew, hOld;
  for (const RenderPsoKey& k : keys) {
    hNew.push_back(k.hash());
    hOld.push_back(fnvHash(k));
  }
  const size_t collNew = keys.size() - std::set<size_t>(hNew.begin(), hNew.end()).size();
  const size_t collOld = keys.size() - std::set<size_t>(hOld.begin(), hOld.end()).size();
  std::printf("real keys: %zu distinct, full collisions new=%zu old=%zu\n", keys.size(), collNew, collOld);
  CHECK(collNew == 0u);
  if (keys.size() < 50u)
    return;
  for (size_t m : { size_t(13u), size_t(97u), size_t(1031u), size_t(8191u) }) {
    const double xn = chiSquare(hNew, m), xo = chiSquare(hOld, m);
    // A random hash: chi-square ~ (m - 1) +- sqrt(2 (m - 1)); 4 sigma.
    const double bound = double(m - 1u) + 4.0 * std::sqrt(2.0 * double(m - 1u));
    std::printf("  buckets %5zu: chi2 new %.1f old %.1f (random-hash bound %.1f)\n", m, xn, xo, bound);
    CHECK(xn <= std::max(xo, bound));
  }
}

static void timing() {
  std::mt19937_64 rng(7u);
  std::vector<RenderPsoKey> ks;
  for (int i = 0; i < 64; i++)
    ks.push_back(randomKey(rng));
  volatile size_t sink = 0u;
  for (int pass = 0; pass < 2; pass++) {
    const auto t0 = std::chrono::steady_clock::now();
    size_t acc = 0u;
    for (int r = 0; r < 20000; r++)
      for (const RenderPsoKey& k : ks)
        acc += pass ? fnvHash(k) : k.hash();
    const auto t1 = std::chrono::steady_clock::now();
    sink = sink + acc;
    std::printf("timing: %s %.1f ns per hash (native)\n", pass ? "old byte-wise FNV" : "new word-wise",
      double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) / (20000.0 * 64.0));
  }
}

int main(int argc, char** argv) {
  testAvalanche();
  std::vector<RenderPsoKey> keys;
  for (int i = 1; i < argc; i++) {
    auto k = readKeys(argv[i]);
    keys.insert(keys.end(), k.begin(), k.end());
  }
  if (!keys.empty())
    testRealKeys(keys);
  timing();
  if (g_failures) {
    std::printf("pso_key_hash_test: %d FAILED\n", g_failures);
    return 1;
  }
  std::printf("pso_key_hash_test: OK\n");
  return 0;
}
