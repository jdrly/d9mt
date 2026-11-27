// Native test for the sampler LOD bias (driver/backend/mtl_sampler_bias.h) and for the bias that
// MtlDeviceImpl::createSampler hands the bridge. scripts/test-sampler-lod-bias.sh cuts
// createSampler's fill of WMTSamplerInfo out of mtl_device_impl.cpp (sampler_fill_extract.py)
// into sampler_fill.inc, included below, and runs it against a fake MTLDevice_newSamplerState
// that keeps the struct it gets. Before the fix the bias never reached the struct, so the fill
// cases fail on such a commit (scripts/test-sampler-lod-bias.sh <rev>).
//
// The helper's rules go through one table each. Stubs that get one rule wrong run through the
// same tables and must fail them: a table they passed wouldn't test that rule.
//
//   scripts/test-sampler-lod-bias.sh [<rev>]
#include "winemetal.h"
#include "mtl_sampler_bias.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace dxvk;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// The layout both sides of the bridge read: the bias after gpu_resource_id.
static_assert(sizeof(WMTSamplerInfo) == 40, "WMTSamplerInfo's size");
static_assert(offsetof(WMTSamplerInfo, gpu_resource_id) == 24, "gpu_resource_id's offset");
static_assert(offsetof(WMTSamplerInfo, lod_bias) == 32, "lod_bias's offset");

static uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, sizeof(u)); return u; }

// ---- The mode: GPU family and D9MT_SAMPLER_LOD_BIAS.

using ModeFn = SamplerLodBiasMode (*)(bool, const char*, bool&);
constexpr SamplerLodBiasMode On = SamplerLodBiasMode::On, OffGpu = SamplerLodBiasMode::OffGpu,
                             OffEnv = SamplerLodBiasMode::OffEnv;

struct ModeCase { bool apple10; const char* env; SamplerLodBiasMode mode; bool understood; };

static const ModeCase kModes[] = {
  { true,  nullptr, On,     true  },
  { false, nullptr, OffGpu, true  },
  { true,  "",      On,     true  },
  { false, "",      OffGpu, true  },
  { true,  "1",     On,     true  },
  { false, "1",     OffGpu, true  },  // nothing turns it on before Apple10
  { true,  "0",     OffEnv, true  },
  { false, "0",     OffEnv, true  },
  { true,  "yes",   On,     false },
  { false, "yes",   OffGpu, false },
  { true,  "00",    On,     false },
  { true,  " 0",    On,     false },
  { true,  "10",    On,     false },
};

static bool passesModes(const char* name, ModeFn fn) {
  for (const ModeCase& c : kModes) {
    bool understood = !c.understood;
    const SamplerLodBiasMode m = fn(c.apple10, c.env, understood);
    if (m != c.mode || understood != c.understood) {
      std::printf("%-16s apple10=%d env=%s: mode %u understood %d (want %u, %d)\n", name, c.apple10,
        c.env ? c.env : "(unset)", unsigned(m), understood, unsigned(c.mode), c.understood);
      return false;
    }
  }
  std::printf("%-16s passes all %zu mode cases\n", name, sizeof(kModes) / sizeof(kModes[0]));
  return true;
}

static SamplerLodBiasMode noFamilyCheck(bool, const char* env, bool& understood) {
  return samplerLodBiasMode(true, env, understood);
}

static SamplerLodBiasMode oneForcesOn(bool apple10, const char* env, bool& understood) {
  const SamplerLodBiasMode m = samplerLodBiasMode(apple10, env, understood);
  return env && !std::strcmp(env, "1") ? On : m;
}

static SamplerLodBiasMode prefixZero(bool apple10, const char* env, bool& understood) {
  const SamplerLodBiasMode m = samplerLodBiasMode(apple10, env, understood);
  if (env && env[0] == '0') { understood = true; return OffEnv; }
  return m;
}

// ---- The bias: clamp, 1/64 steps, NaN and -0.

using BiasFn = float (*)(float, bool);

struct BiasCase { float key; bool on; float sent; };

static const float kNaN = std::numeric_limits<float>::quiet_NaN();

static const BiasCase kBiases[] = {
  { -0.75f,      true,  -0.75f },      // the UI's bias in the brief
  { -0.75f,      false,  0.0f },
  {  1.0f,       true,   1.0f },
  {  1.0f,       false,  0.0f },
  {  0.0f,       true,   0.0f },
  { -0.0f,       true,   0.0f },       // +0: the bridge's != 0 test and the sampler key agree
  {  0.3f,       true,   19.0f / 64.0f },
  { -0.3f,       true,  -19.0f / 64.0f },
  {  0.001f,     true,   0.0f },
  {  100.0f,     true,   1023.0f / 64.0f },
  { -100.0f,     true,  -16.0f },
  {  kNaN,       true,   0.0f },
  {  kNaN,       false,  0.0f },
};

static bool passesBiases(const char* name, BiasFn fn) {
  for (const BiasCase& c : kBiases) {
    const float got = fn(c.key, c.on);
    if (bits(got) != bits(c.sent)) {
      std::printf("%-16s key %g on=%d: %g (want %g)\n", name, double(c.key), c.on, double(got), double(c.sent));
      return false;
    }
  }
  std::printf("%-16s passes all %zu bias cases\n", name, sizeof(kBiases) / sizeof(kBiases[0]));
  return true;
}

static float ignoresOff(float key, bool) { return metalLodBias(key, true); }
static float noClamp(float key, bool on) {
  return on && !std::isnan(key) ? std::round(key * 64.0f) / 64.0f + 0.0f : metalLodBias(key, on);
}
static float noSteps(float key, bool on) {
  return on && !std::isnan(key) ? std::fmin(std::fmax(key, kMtlLodBiasMin), kMtlLodBiasMax) + 0.0f : 0.0f;
}
static float keepsNegZero(float key, bool on) {
  return key == 0.0f ? key : metalLodBias(key, on);
}

// ---- createSampler's fill, the driver's own lines.

static WMTSamplerInfo g_sent;
static int g_calls = 0;

extern "C" obj_handle_t MTLDevice_newSamplerState(obj_handle_t, struct WMTSamplerInfo* info) {
  g_sent = *info;
  g_calls++;
  return 1u;
}

struct FakeKey {
  uint32_t minFilter = 1u, magFilter = 1u, mipMode = 2u;
  uint32_t addressU = 0u, addressV = 1u, addressW = 2u, borderColor = 0u, compareOp = 3u;
  bool     compareEnable = false;
  float    minLod = 2.0f, maxLod = 16.0f;
  uint32_t maxAnisotropy = 4u;
  float    mipLodBias = 0.0f;
};

struct FakeDevice {
  struct { obj_handle_t handle() const { return 7u; } } m_device;
  bool m_samplerLodBias = false;

  WMTSamplerInfo fill(const FakeKey& key) {
    std::memset(&g_sent, 0xA5, sizeof(g_sent));
#include "sampler_fill.inc"
    (void) handle;
    return g_sent;
  }
};

static void fillCases() {
  FakeDevice dev;
  FakeKey key;

  dev.m_samplerLodBias = true;
  key.mipLodBias = -0.75f;
  WMTSamplerInfo s = dev.fill(key);
  CHECK(bits(s.lod_bias) == bits(-0.75f));
  CHECK(s.padding == 0u);
  CHECK(s.lod_min_clamp == 2.0f && s.lod_max_clamp == 16.0f && s.max_anisotroy == 4u);
  CHECK(s.support_argument_buffers && s.normalized_coords);

  key.mipLodBias = 100.0f;
  CHECK(bits(dev.fill(key).lod_bias) == bits(1023.0f / 64.0f));

  key.mipLodBias = 0.0f;
  CHECK(bits(dev.fill(key).lod_bias) == 0u);

  // Before Apple10, or turned off: 0, the struct as before the field existed.
  dev.m_samplerLodBias = false;
  key.mipLodBias = -0.75f;
  s = dev.fill(key);
  CHECK(bits(s.lod_bias) == 0u && s.padding == 0u);

  CHECK(g_calls == 4);
}

int main() {
  CHECK(passesModes("mode", samplerLodBiasMode));
  CHECK(!passesModes("no family check", noFamilyCheck));
  CHECK(!passesModes("1 forces on", oneForcesOn));
  CHECK(!passesModes("prefix 0", prefixZero));

  CHECK(passesBiases("bias", metalLodBias));
  CHECK(!passesBiases("ignores off", ignoresOff));
  CHECK(!passesBiases("no clamp", noClamp));
  CHECK(!passesBiases("no 1/64 steps", noSteps));
  CHECK(!passesBiases("keeps -0", keepsNegZero));

  // Each mode's line names it.
  CHECK(std::strstr(samplerLodBiasLine(On), "on (Apple10 GPU)") != nullptr);
  CHECK(std::strstr(samplerLodBiasLine(OffGpu), "off (GPU before Apple10)") != nullptr);
  CHECK(std::strstr(samplerLodBiasLine(OffEnv), "off (D9MT_SAMPLER_LOD_BIAS=0)") != nullptr);

  fillCases();

  std::printf("sampler_lod_bias_test: %d failed\n", g_fail);
  return g_fail ? 1 : 0;
}
