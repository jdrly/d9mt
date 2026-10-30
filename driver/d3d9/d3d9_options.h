#pragma once

#include "../util/config/config.h"
#include "../util/util_likely.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk { class MtlDevice; }

namespace dxvk {

  enum class D3D9FloatEmulation : uint8_t {
    Disabled = 0,
    Enabled  = 1,
    Strict   = 2
  };

  struct D3D9Options {

    D3D9Options(const Rc<MtlDevice>& device, const Config& config);

    int32_t customVendorId;
    int32_t customDeviceId;
    std::string customDeviceDesc;

    bool hideNvidiaGpu;

    bool hideNvkGpu;

    bool hideAmdGpu;

    bool hideIntelGpu;

    int32_t presentInterval;

    int32_t maxFrameLatency;

    int32_t maxFrameRate;

    uint32_t shaderModel;

    bool dpiAware;

    bool lenientClear;

    bool allowOutOfRangeVsDefs;

    bool deferSurfaceCreation;

    int32_t samplerAnisotropy;

    // MB; -1 (default) = the memory profile's reportedVramMB (d9mt.reportedVramMB)
    int32_t maxAvailableMemory;

    D3D9FloatEmulation d3d9FloatEmulation;

    bool useFP16;

    bool supportCubeDepthFormats;

    bool supportDFFormats;

    bool supportX4R4G4B4;

    bool useD32forD24;

    // works around a Sims 2 game bug that also occurs on native drivers
    bool disableA8RT;

    bool memoryTrackTest;

    std::string forceAspectRatio;

    uint32_t forceRefreshRate;

    bool modeCountCompatibility;

    // works around a Halo CE bug that hands cube textures to 2d/volume samplers
    bool forceSamplerTypeSpecConstants;

    bool forceSampleRateShading;

    bool allowDiscard;

    bool enumerateByDisplays;

    bool cachedWriteOnlyBuffers;

    Tristate deviceLocalConstantBuffers;

    bool allowDirectBufferMapping;

    // Map DEFAULT + WRITEONLY buffers directly (upstream DXVK); false keeps
    // them as a GPU buffer plus a full-size CPU staging copy.
    bool writeOnlyDirectMapping;

    // Stamp DIRECT buffers' uses per CS chunk so in-place locks synchronize
    // only up to their last use (off in the legacy profile unless
    // writeOnlyDirectMapping is forced on).
    bool trackDirectBufferUses;

    // A DIRECT DEFAULT+WRITEONLY buffer locked in place (no DISCARD/NOOVERWRITE)
    // more than this many times after its backing was used goes back to
    // BUFFER mode; 0 = never.
    uint32_t writeOnlyDemoteLocks;

    // some Source engine games write to D3DPOOL_DEFAULT buffers after unlocking them;
    // uploading on unlock would lose those writes, so upload at draw time instead
    bool forceDrawTimeBufferUpload;

    bool ignoreDefaultBufferLockRange;

    bool seamlessCubes;

    float samplerLodBias;

    bool clampNegativeLodBias;

    int32_t textureMemory;

    std::string shaderDumpPath;

    bool deviceLossOnFocusLoss;

    bool countLosableResources;

    bool reproducibleCommandStream;

    /// D9MT_FLUSH_CAP=1: cap heuristic flushes at ImplicitStrongHint, as upstream does on
    /// tiling GPUs (GetMaxFlushType; reproducibleCommandStream takes precedence). Off by
    /// default. Kept next to reproducibleCommandStream, in its padding.
    bool flushCapStrong;

    int32_t drefScaling;

    bool extraFrontbuffer;

    /// d9mt.stateCache: pipeline state cache (docs/state-cache.md);
    /// D9MT_STATE_CACHE=0 turns it off too.
    bool stateCache;
  };

}
