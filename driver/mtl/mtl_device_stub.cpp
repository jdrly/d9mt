#include <cstring>

#include "mtl_device.h"

#include "../wsi/wsi_platform.h"
#include "mtl_context.h"
#include "mtl_state_cache.h"

#include "../util/log/log.h"
#include "../util/util_env.h"
#include "../util/config/config.h"

namespace dxvk {

  // Field by field, not over the bytes: the padding after compareEnable is not initialized
  // by whoever fills the key, so equal keys could hash and compare different.
  static uint32_t floatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
  }

  size_t MtlSamplerKey::hash() const {
    const uint32_t words[] = {
      uint32_t(magFilter), uint32_t(minFilter), uint32_t(mipMode),
      uint32_t(addressU), uint32_t(addressV), uint32_t(addressW), maxAnisotropy,
      floatBits(mipLodBias), floatBits(minLod), floatBits(maxLod),
      uint32_t(compareOp), uint32_t(compareEnable), uint32_t(borderColor) };
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t w : words)
      h = (h ^ w) * 0x100000001b3ull;
    return size_t(h ^ (h >> 32));
  }

  bool MtlSamplerKey::eq(const MtlSamplerKey& other) const {
    return magFilter == other.magFilter && minFilter == other.minFilter && mipMode == other.mipMode
        && addressU == other.addressU && addressV == other.addressV && addressW == other.addressW
        && maxAnisotropy == other.maxAnisotropy
        && floatBits(mipLodBias) == floatBits(other.mipLodBias)
        && floatBits(minLod) == floatBits(other.minLod) && floatBits(maxLod) == floatBits(other.maxLod)
        && compareOp == other.compareOp && compareEnable == other.compareEnable
        && borderColor == other.borderColor;
  }

  const char* memCategoryName(MtlMemCategory c) {
    static const char* const names[] = {
      "other", "texManaged", "texDefault", "texSysmem", "texDynamic", "texRT", "texDS",
      "texMapManaged", "texMap", "vbibGpu", "vbibStaging", "vbibDirect",
      "constBuf", "staging", "up" };
    static_assert(sizeof(names) / sizeof(names[0]) == size_t(MtlMemCategory::Count));
    return uint32_t(c) < uint32_t(MtlMemCategory::Count) ? names[uint32_t(c)] : "?";
  }

  MtlDevice::MtlDevice() { }
  MtlDevice::~MtlDevice() { }

  void MtlDevice::setStateCache(const Rc<MtlStateCache>& cache) {
    m_stateCache = cache;
    m_stateCachePtr.store(cache.ptr(), std::memory_order_release);
  }

  Rc<MtlBuffer> MtlDevice::createBuffer(const MtlBufferCreateInfo& info) {
    Logger::warn("MtlDevice::createBuffer: stub");
    return nullptr;
  }

  Rc<MtlTexture> MtlDevice::createImage(const MtlImageCreateInfo& info) {
    Logger::warn("MtlDevice::createImage: stub");
    return nullptr;
  }

  Rc<MtlTextureView> MtlDevice::createImageView(
    const Rc<MtlTexture>& image, const MtlImageViewKey& key) {
    Logger::warn("MtlDevice::createImageView: stub");
    return nullptr;
  }

  Rc<MtlSamplerState> MtlDevice::createSampler(const MtlSamplerKey& key) {
    Logger::warn("MtlDevice::createSampler: stub");
    return nullptr;
  }

  Rc<MtlShaderFunction> MtlDevice::createShaderFunction(
    MtlShaderStage stage, const void* metallib, size_t size, const void* fallback, size_t fallbackSize) {
    Logger::warn("MtlDevice::createShaderFunction: stub");
    return nullptr;
  }

  void MtlDevice::submitCompile(const Rc<MtlCompileTask>& task, bool urgent) { }

  size_t MtlDevice::pendingCompiles() {
    return 0u;
  }

  Rc<MtlContext> MtlDevice::createContext() {
    Logger::warn("MtlDevice::createContext: stub");
    return nullptr;
  }

  uint64_t MtlDevice::getCompletedSerial() {
    return 0u;
  }

  uint64_t MtlDevice::getNextSerial() {
    return 1u;
  }

  void MtlDevice::waitForIdle() {
    Logger::warn("MtlDevice::waitForIdle: stub");
  }

  bool MtlDevice::supportsFormat(MtlFormat format, MtlUsage usage) const {
    return format != MtlFormat::Invalid;
  }

  const MtlDeviceInfo& MtlDevice::deviceInfo() const {
    return m_info;
  }

  Rc<MtlAllocation> MtlDevice::renameBuffer(MtlBuffer* buffer) {
    Logger::warn("MtlDevice::renameBuffer: stub");
    return nullptr;
  }

  MtlStageMask MtlDevice::getShaderStages() const {
    return MtlStageVertex | MtlStageFragment;
  }

  void MtlDevice::waitForResource(const MtlResource& resource) {
    (void) resource;
    Logger::warn("MtlDevice::waitForResource: stub");
  }

  Rc<MtlStagingBuffer> MtlDevice::createStagingBuffer(MtlDeviceSize size) {
    MtlBufferCreateInfo info = { };
    info.size = size;
    info.usage = MtlUsage::Staging;
    info.cpuAccess = MtlCpuAccess::Write;
    info.category = MtlMemCategory::Staging;
    Rc<MtlBuffer> buffer = createBuffer(info);
    return buffer != nullptr ? new MtlStagingBuffer(std::move(buffer), size) : nullptr;
  }

  MtlStagingBufferStats MtlDevice::getStagingMemoryStatistics() {
    return MtlStagingBufferStats();
  }

  MtlGpuQueryStatus MtlQuery::getData(MtlQueryData& out) {
    out = MtlQueryData{};
    return MtlGpuQueryStatus::Available;
  }

  MtlGpuEventStatus MtlEvent::test() {
    return MtlGpuEventStatus::Signaled;
  }

  Rc<MtlQuery> MtlDevice::createGpuQuery(MtlQueryType type) {
    Rc<MtlQuery> q = new MtlQuery();
    q->type = type;
    return q;
  }

  Rc<MtlEvent> MtlDevice::createGpuEvent() {
    return new MtlEvent();
  }

  uint64_t MtlDevice::getTimestampFrequency() const {
    return 1000000000ull;
  }

  uint64_t MtlDevice::allocatedBytes() {
    return 0u;
  }

  const MtlDeviceInfo& MtlAdapter::deviceInfo() const {
    return m_info;
  }

  bool MtlAdapter::supportsFormat(MtlFormat format, MtlUsage usage) const {
    return format != MtlFormat::Invalid;
  }

  Rc<MtlDevice> MtlAdapter::createDevice() {
    extern Rc<MtlDevice> backendCreateDevice(const MtlMemoryProfile& profile);
    Rc<MtlDevice> device = backendCreateDevice(m_memProfile);
    if (device == nullptr) {
      Logger::err("MtlAdapter::createDevice: backend device unavailable");
      return new MtlDevice();
    }
    return device;
  }

  MtlInstance::MtlInstance() {
    wsi::init();

    m_config.merge(Config::getUserConfig());
    m_config.merge(Config::getAppConfig(env::getExePath()));
    m_config.logOptions();

    m_memProfile = resolveMemoryProfile(m_config);
  }

  MtlInstance::~MtlInstance() {
    wsi::quit();
  }

  uint32_t MtlInstance::adapterCount() const {
    return 1u;
  }

  Rc<MtlAdapter> MtlInstance::enumAdapters(uint32_t index) {
    return index == 0u ? new MtlAdapter(m_memProfile) : nullptr;
  }

}
