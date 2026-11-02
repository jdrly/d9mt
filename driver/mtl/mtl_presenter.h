#pragma once

#include <cstdint>

#include "mtl_types.h"
#include "mtl_resource.h"

#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk {

  class MtlDevice;

  struct MtlAcquiredDrawable {
    Rc<MtlTexture>  texture = nullptr;
    Rc<MtlTextureView> view = nullptr;
    MtlObjectHandle drawable = MtlNullHandle;
    MtlPresenterSync sync   = { };
    uint32_t        imageIndex = 0u;
    bool            valid   = false;
  };

  // D9MT_PROFILE (SCREEN line, d9mt #153): when a tracked drawable reached the screen.
  struct MtlPresentedRecord {
    uint64_t seq        = 0u;
    double   submitS    = 0.0; // host time, s, when present was called
    double   presentedS = 0.0; // MTLDrawable.presentedTime; 0 = never shown
  };

  struct MtlScreenState {
    bool     displaySync   = true;
    uint32_t drawables     = 0u;  // CAMetalLayer.maximumDrawableCount
    bool     windowValid   = false;
    bool     fullscreen    = false;
    double   screenMaxFps  = 0.0;
    double   minIntervalS  = 0.0;
    double   maxIntervalS  = 0.0;
  };

  struct MtlPresenterDesc {
    MtlExtent2D imageExtent  = { };
    MtlFormat   imageFormat  = MtlFormat::BGRA8Unorm;
    uint32_t    imageCount   = 3u;
    bool        vsync        = true;
    intptr_t    hwnd         = 0;
  };

  class MtlPresenter : public RcObject {

  public:

    virtual ~MtlPresenter() { }

    virtual void recreate(const MtlPresenterDesc& desc) = 0;

    virtual void setSyncInterval(uint32_t syncInterval) = 0;

    // Must be called every Present: macdrv resizes the NSView asynchronously,
    // so a size latched at create/Reset goes stale for an unknown number of
    // frames, and sizing from the backbuffer instead of the live contentsScale
    // renders into the top-left quarter on Retina (scale 2).
    virtual void syncLayerGeometry(uint32_t clientWidth, uint32_t clientHeight) = 0;

    virtual MtlAcquiredDrawable acquireNextImage(uint64_t frameId) = 0;

    virtual void present(const MtlAcquiredDrawable& image) = 0;

    virtual void signalFrameLatency(uint64_t frameId) = 0;

    virtual MtlExtent2D getImageExtent() const = 0;

    // False when macOS isn't compositing the layer (occluded, other Space,
    // miniaturized): presented drawables never retire while uncomposited, so
    // acquiring anyway drains the pool and nextDrawable starts blocking ~1s
    // per frame, a backlog that persists after the window returns. Win32
    // focus doesn't substitute for this — it doesn't track Spaces.
    virtual bool isVisible() = 0;

    // Display refresh rate (Hz, 0 = unknown); bounds how long the CS thread
    // waits for a drawable before skipping the present.
    virtual void setRefreshRate(double hz) { (void)hz; }

    // True while presents are being skipped because no drawable arrived in
    // time (WindowServer not latching). The app thread paces Present to the
    // display rate meanwhile, since nothing else does.
    virtual bool isDrawableStalled() const { return false; }

    // D9MT_PROFILE only: copies up to `max` presented records out of this presenter's stream (the
    // render thread presents with a presented handler while profiling), strictly in present
    // order; `pending`: the stream's presents not yet retired, `lostNew`: presents this call
    // retired as lost.
    virtual uint32_t drainPresented(MtlPresentedRecord* out, uint32_t max,
                                    uint64_t* pending, uint64_t* lostNew) {
      (void)out; (void)max; *pending = 0u; *lostNew = 0u; return 0u;
    }

    // D9MT_PROFILE only: the layer's and its window's present-relevant state (never blocks).
    virtual MtlScreenState screenState() { return MtlScreenState(); }

    // D9MT_PROFILE only: identifies this presenter's stream (unique in the process, never reused;
    // 0 = no stream), so the SCREEN window breaks its interval chain when the presenter changes.
    virtual uint64_t screenStreamId() const { return 0u; }

    // D9MT_PROFILE only, CS thread: the next tracked present puts a break marker into the stream
    // first (a window switch back to this presenter, or presents skipped in between).
    virtual void markScreenBreak() { }
  };

  Rc<MtlPresenter> createMtlPresenter(const Rc<MtlDevice>& device, const MtlPresenterDesc& desc);

}
