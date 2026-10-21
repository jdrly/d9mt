#include "mtl_presenter_impl.h"

#include "mtl_device_impl.h"

#include "../mtl/mtl_context.h"
#include "../util/log/log.h"
#include "../util/thread.h"
#include "../util/util_env.h"
#include "../util/util_log_limit.h"
#include "../util/util_string.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace dxvk::backend {

  namespace {
    using Clock = std::chrono::steady_clock;

    WMTPixelFormat wmtFormat(MtlFormat f) { return WMTPixelFormat(uint32_t(f)); }

    // nextDrawable calls slower than this are logged by the acquirer thread.
    constexpr int64_t kSlowAcquireLogMs = 50;
    // A geometry change the main thread hasn't applied yet is re-sent after this.
    constexpr int64_t kGeometryResendMs = 500;
    constexpr uint64_t kVisUnknown      = ~0ull;

    int64_t msSince(Clock::time_point t0, Clock::time_point t1) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    }

    // D9MT_DRAWABLE_TIMEOUT_MS=<n>: max CS-thread wait for a drawable
    // (default max(3 refresh intervals, 50 ms)).
    int64_t drawableTimeoutOverrideMs() {
      static const int64_t s_ms = [] {
        std::string v = env::getEnvVar("D9MT_DRAWABLE_TIMEOUT_MS");
        return v.empty() ? int64_t(-1) : int64_t(std::strtol(v.c_str(), nullptr, 10));
      } ();
      return s_ms;
    }
  }

  class MtlPresenterImpl : public MtlPresenter {

  public:

    MtlPresenterImpl(const Rc<MtlDevice>& device, intptr_t hwnd, const MtlPresenterDesc& desc)
    : m_device(device), m_desc(desc) {
      m_backend   = static_cast<MtlDeviceImpl*>(device.ptr());
      m_wmtDevice = m_backend ? m_backend->wmtDevice() : 0u;

      obj_handle_t layer = 0u;
      m_view  = CreateMetalViewFromHWND(hwnd, m_wmtDevice, &layer);
      m_layer = layer;
      if (!m_layer)
        Logger::err("MtlPresenterImpl: CreateMetalViewFromHWND returned no layer");
      setRefreshRate(0.0);
      applyProps("create");

      attachLayerResidency();
    }

    ~MtlPresenterImpl() {
      stopAcquirer();
      if (m_backend && m_layer)
        m_backend->residency().detachLayer(m_layer);
      if (m_view)
        ReleaseMetalView(m_view);
    }

    void recreate(const MtlPresenterDesc& desc) override {
      { std::lock_guard<dxvk::mutex> lock(m_descMutex);
        m_desc = desc;
      }
      // A parked drawable has the old size/format.
      dropParkedDrawable();
      applyProps("recreate");
    }

    void setSyncInterval(uint32_t syncInterval) override {
      bool vsync = syncInterval != 0u;

      if (m_desc.vsync != vsync) {
        { std::lock_guard<dxvk::mutex> lock(m_descMutex);
          m_desc.vsync = vsync;
        }
        applyProps(vsync ? "vsync on" : "vsync off");
      }
    }

    void syncLayerGeometry(uint32_t clientWidth, uint32_t clientHeight) override {
      if (!m_layer || !clientWidth || !clientHeight)
        return;

      /* Wine's client rect is already in backing pixels, so it IS the
       * drawable size — scaling it again would double the drawable. */
      WMTLayerProps cur = { };
      MetalLayer_getProps(m_layer, &cur);

      double w = double(clientWidth);
      double h = double(clientHeight);

      if (cur.drawable_width == w && cur.drawable_height == h) {
        m_geomReqW = m_geomReqH = 0.0;
        m_geomSeenW = w;
        m_geomSeenH = h;
        return;
      }

      // setProps applies on the main thread asynchronously: while it is
      // queued (main thread busy) the layer still reports the old size, so
      // don't queue another full property set every frame.
      auto now = Clock::now();
      if (m_geomReqW == w && m_geomReqH == h
       && msSince(m_geomReqAt, now) < kGeometryResendMs)
        return;

      // The layer changed size without us asking (AppKit/winemac resize).
      bool external = m_geomSeenW != 0.0
        && (cur.drawable_width != m_geomSeenW || cur.drawable_height != m_geomSeenH);

      logLayer(str::format("MtlPresenter: drawableSize ", cur.drawable_width, "x", cur.drawable_height,
        " -> ", clientWidth, "x", clientHeight,
        external ? " (layer was resized externally)" : "",
        m_geomReqW != 0.0 ? " (previous request not applied yet)" : ""));

      m_geomReqW  = w;
      m_geomReqH  = h;
      m_geomReqAt = now;
      m_geomSeenW = m_geomSeenH = 0.0;

      WMTLayerProps props = cur;
      props.drawable_width  = w;
      props.drawable_height = h;
      MetalLayer_setProps(m_layer, &props);
    }

    MtlAcquiredDrawable acquireNextImage(uint64_t frameId) override {
      // CS thread; m_desc is written on the app thread (recreate, setSyncInterval).
      MtlPresenterDesc desc;
      { std::lock_guard<dxvk::mutex> lock(m_descMutex);
        desc = m_desc;
      }
      MtlAcquiredDrawable image = {};
      image.sync.frameId = frameId;
      image.imageIndex   = uint32_t(frameId % (desc.imageCount ? desc.imageCount : 1u));

      if (!m_layer)
        return image;

      attachLayerResidency();

      obj_handle_t drawable = takeDrawable(frameId);
      if (!drawable)
        return image;

      image.drawable = drawable;
      image.sync.drawable = drawable;

      Rc<MtlTextureImpl> tex = new MtlTextureImpl();
      obj_handle_t drawableTex = MetalDrawable_texture(drawable);
      tex->texture.tex = drawableTex;
      tex->format      = desc.imageFormat;
      uint32_t dw = uint32_t(MTLTexture_width(drawableTex));
      uint32_t dh = uint32_t(MTLTexture_height(drawableTex));
      tex->extent      = { dw ? dw : desc.imageExtent.width,
                           dh ? dh : desc.imageExtent.height, 1u };
      tex->m_info.type        = MtlImageType::e2D;
      tex->m_info.format      = desc.imageFormat;
      tex->m_info.extent      = { tex->extent.width, tex->extent.height, 1u };
      tex->m_info.mipLevels   = 1u;
      tex->m_info.numLayers   = 1u;
      tex->m_info.sampleCount = 1u;
      tex->m_info.usage       = MtlTextureUsageRenderTarget;

      Rc<MtlTextureViewImpl> view = new MtlTextureViewImpl();
      view->owned      = WmtTexture::retain(drawableTex);
      view->view.tex   = drawableTex;
      view->image      = tex.ptr();
      view->key.format = desc.imageFormat;

      image.texture    = tex;
      image.view       = view;
      image.valid      = true;
      return image;
    }

    void present(const MtlAcquiredDrawable& image) override {
      m_lastPresented = image.sync.frameId;
    }

    void signalFrameLatency(uint64_t frameId) override {
      /* Must advance every frame — the swapchain's frame limiter waits on
       * this monotonically-increasing counter and deadlocks otherwise. */
      if (frameId > m_completedFrame)
        m_completedFrame = frameId;
    }

    MtlExtent2D getImageExtent() const override { return m_desc.imageExtent; }

    bool isVisible() override {
      if (!m_view)
        return true;

      uint64_t vis  = MetalView_visibility(m_view);
      uint64_t prev = m_lastVis.exchange(vis);

      if (unlikely(prev != vis && prev != kVisUnknown)) {
        std::lock_guard<dxvk::mutex> lock(m_logMutex);
        uint32_t dropped = 0u;
        if (m_visLog.allow(&dropped)) {
          Logger::warn(str::format("MtlPresenter: ", wallClockStamp(),
            " visibility 0x", std::hex, prev, " -> 0x", vis, std::dec,
            " (visible=", (vis & WMT_VISIBILITY_VISIBLE) != 0u,
            " activeSpace=", (vis & WMT_VISIBILITY_ON_ACTIVE_SPACE) != 0u,
            " miniaturized=", (vis & WMT_VISIBILITY_MINIATURIZED) != 0u, ")",
            dropped ? str::format(" [", dropped, " earlier lines suppressed]") : std::string()));
        }
      }

      return (vis & WMT_VISIBILITY_VISIBLE)
          && (vis & WMT_VISIBILITY_ON_ACTIVE_SPACE)
          && !(vis & WMT_VISIBILITY_MINIATURIZED);
    }

    void setRefreshRate(double hz) override {
      int64_t overrideMs = drawableTimeoutOverrideMs();
      int64_t us;

      if (overrideMs > 0) {
        us = overrideMs * 1000;
      } else {
        if (!(hz >= 20.0 && hz <= 1000.0))
          hz = 60.0;
        // Generous enough for a GPU-bound frame plus a vblank; a WindowServer
        // that stopped latching is held for ~1 s, far beyond this.
        us = std::max(int64_t(3.0e6 / hz), int64_t(50000));
      }

      m_acquireTimeoutUs.store(us);
    }

    bool isDrawableStalled() const override {
      return m_stalledFlag.load(std::memory_order_relaxed);
    }

  private:

    Rc<MtlDevice>    m_device;
    MtlPresenterDesc m_desc;      // written on the app thread only; m_descMutex for the CS reader
    dxvk::mutex      m_descMutex;
    MtlDeviceImpl*   m_backend   = nullptr;
    obj_handle_t     m_wmtDevice = 0u;
    obj_handle_t     m_view      = 0u;
    obj_handle_t     m_layer     = 0u;
    bool             m_layerResidencyAttached = false;
    uint64_t         m_lastPresented = 0u;
    uint64_t         m_completedFrame = 0u;

    // Drawable acquisition runs on its own thread so that a WindowServer
    // that stops latching (nextDrawable then blocks until Core Animation's
    // 1 s timeout) never blocks the CS thread. At most one request is
    // outstanding and at most one drawable is parked, so the number of
    // acquired-but-unpresented drawables stays at one, as before.
    dxvk::thread              m_acqThread;
    dxvk::mutex               m_acqMutex;
    dxvk::condition_variable  m_acqCond;      // acquirer: request / stop
    dxvk::condition_variable  m_acqDoneCond;  // CS: result ready
    bool                      m_acqStop    = false;
    bool                      m_acqPending = false;
    bool                      m_acqReady   = false;
    obj_handle_t              m_acqDrawable = 0u;
    Clock::time_point         m_acqRequestedAt = { };
    std::atomic<int64_t>      m_acquireTimeoutUs = { 50000 };

    // CS thread only (except m_stalledFlag).
    bool                      m_stalled = false;
    Clock::time_point         m_stallStart = { };
    uint64_t                  m_stallSkipped = 0u;
    std::atomic<bool>         m_stalledFlag = { false };
    LogLimiter                m_stallLog = { 20u, std::chrono::seconds(10) };
    LogLimiter                m_acqLog   = { 20u, std::chrono::seconds(10) };

    std::atomic<uint64_t>     m_lastVis = { kVisUnknown };
    dxvk::mutex               m_logMutex;
    LogLimiter                m_visLog   = { 40u, std::chrono::seconds(10) };
    LogLimiter                m_layerLog = { 20u, std::chrono::seconds(10) };

    // App thread only (syncLayerGeometry / applyProps).
    double                    m_geomReqW  = 0.0;
    double                    m_geomReqH  = 0.0;
    Clock::time_point         m_geomReqAt = { };
    double                    m_geomSeenW = 0.0;
    double                    m_geomSeenH = 0.0;

    void attachLayerResidency() {
      if (m_layerResidencyAttached || !m_backend || !m_layer)
        return;
      m_layerResidencyAttached = m_backend->residency().attachLayer(m_layer);
    }

    void logLayer(const std::string& msg) {
      std::lock_guard<dxvk::mutex> lock(m_logMutex);
      uint32_t dropped = 0u;
      if (m_layerLog.allow(&dropped)) {
        Logger::warn(str::format(msg, " at ", wallClockStamp(),
          dropped ? str::format(" [", dropped, " earlier lines suppressed]") : std::string()));
      }
    }

    void startAcquirer() {
      if (m_acqThread.joinable())
        return;
      m_acqThread = dxvk::thread([this] { acquirerLoop(); });
    }

    void stopAcquirer() {
      if (!m_acqThread.joinable())
        return;

      // Process exit: the thread is already gone (possibly holding the lock).
      if (this_thread::isInModuleDetachment()) {
        m_acqThread.detach();
        return;
      }

      { std::lock_guard<dxvk::mutex> lock(m_acqMutex);
        m_acqStop = true;
        m_acqCond.notify_all();
      }

      // May wait out an in-flight nextDrawable (<= ~1 s).
      m_acqThread.join();
      dropParkedDrawable();
    }

    void dropParkedDrawable() {
      obj_handle_t drawable = 0u;

      { std::lock_guard<dxvk::mutex> lock(m_acqMutex);
        if (m_acqReady) {
          drawable    = m_acqDrawable;
          m_acqDrawable = 0u;
          m_acqReady  = false;
        }
      }

      // Releasing an unpresented drawable returns it to the layer's pool.
      if (drawable)
        NSObject_release(drawable);
    }

    void acquirerLoop() {
      env::setThreadName("d9mt-drawable");

      std::unique_lock<dxvk::mutex> lock(m_acqMutex);

      while (true) {
        m_acqCond.wait(lock, [this] { return m_acqStop || (m_acqPending && !m_acqReady); });

        if (m_acqStop)
          break;

        Clock::time_point requested = m_acqRequestedAt;
        lock.unlock();

        auto t0 = Clock::now();
        obj_handle_t drawable = MetalLayer_nextDrawable(m_layer);
        auto t1 = Clock::now();
        int64_t ms = msSince(t0, t1);

        if (unlikely(ms >= kSlowAcquireLogMs)) {
          uint32_t dropped = 0u;
          if (m_acqLog.allow(&dropped)) {
            Logger::warn(str::format("MtlPresenter: ", wallClockStamp(),
              " nextDrawable blocked ", ms, " ms (got=", drawable != 0u,
              ", requested ", msSince(requested, t1), " ms ago)",
              dropped ? str::format(" [", dropped, " earlier lines suppressed]") : std::string()));
          }
        }

        lock.lock();
        m_acqDrawable = drawable;
        m_acqReady    = true;
        m_acqPending  = false;
        m_acqDoneCond.notify_all();
      }
    }

    // CS thread. Returns a retained drawable, or 0 to skip this present.
    obj_handle_t takeDrawable(uint64_t frameId) {
      startAcquirer();

      auto t0 = Clock::now();
      obj_handle_t drawable = 0u;
      bool gotResult = false;
      int64_t pendingMs = 0;

      { std::unique_lock<dxvk::mutex> lock(m_acqMutex);

        if (!m_acqReady && !m_acqPending) {
          m_acqPending     = true;
          m_acqRequestedAt = t0;
          m_acqCond.notify_one();
        }

        // While stalled, don't wait at all: the app thread paces Present to
        // the display rate, and the drawable is used once it arrives.
        if (!m_acqReady && !m_stalled) {
          auto deadline = t0 + std::chrono::microseconds(m_acquireTimeoutUs.load());

          while (!m_acqReady) {
            auto now = Clock::now();
            if (now >= deadline)
              break;
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                      + std::chrono::milliseconds(1);
            m_acqDoneCond.wait_for(lock, left);
          }
        }

        if (m_acqReady) {
          gotResult     = true;
          drawable      = m_acqDrawable;
          m_acqDrawable = 0u;
          m_acqReady    = false;
        } else {
          pendingMs = msSince(m_acqRequestedAt, Clock::now());
        }
      }

      auto t1 = Clock::now();

      if (drawable) {
        if (unlikely(m_stalled)) {
          m_stalled = false;
          m_stalledFlag.store(false, std::memory_order_relaxed);
          uint32_t dropped = 0u;
          if (m_stallLog.allow(&dropped)) {
            Logger::warn(str::format("MtlPresenter: ", wallClockStamp(),
              " drawable stall over after ", msSince(m_stallStart, t1), " ms (",
              m_stallSkipped, " presents skipped, frame ", frameId, ")",
              dropped ? str::format(" [", dropped, " earlier lines suppressed]") : std::string()));
          }
        }
        return drawable;
      }

      if (!m_stalled) {
        m_stalled      = true;
        m_stallStart   = t0;
        m_stallSkipped = 0u;
        m_stalledFlag.store(true, std::memory_order_relaxed);
        uint32_t dropped = 0u;
        if (m_stallLog.allow(&dropped)) {
          Logger::warn(str::format("MtlPresenter: ", wallClockStamp(),
            " no drawable for frame ", frameId, " after ", msSince(t0, t1), " ms (",
            gotResult ? "nextDrawable timed out" : str::format("request pending ", pendingMs, " ms"),
            "), skipping presents and pacing Present to the display rate",
            dropped ? str::format(" [", dropped, " earlier lines suppressed]") : std::string()));
        }
      }

      m_stallSkipped += 1u;
      return 0u;
    }

    void applyProps(const char* why) {
      if (!m_layer)
        return;
      WMTLayerProps props = {};
      props.device               = m_wmtDevice;
      props.contents_scale       = 0.0;
      props.drawable_width       = double(m_desc.imageExtent.width);
      props.drawable_height      = double(m_desc.imageExtent.height);
      props.opaque               = true;
      props.display_sync_enabled = m_desc.vsync;
      /* The present path copies into the drawable via a compute encoder
       * (MTL4 has no blit encoder); a framebufferOnly drawable rejects that. */
      props.framebuffer_only     = false;
      props.pixel_format         = wmtFormat(m_desc.imageFormat);

      logLayer(str::format("MtlPresenter: layer props (", why, "): drawable ",
        m_desc.imageExtent.width, "x", m_desc.imageExtent.height,
        " vsync=", m_desc.vsync, " format=", uint32_t(m_desc.imageFormat)));

      if (props.drawable_width > 0.0 && props.drawable_height > 0.0) {
        m_geomReqW  = props.drawable_width;
        m_geomReqH  = props.drawable_height;
        m_geomReqAt = Clock::now();
        m_geomSeenW = m_geomSeenH = 0.0;
      }

      MetalLayer_setProps(m_layer, &props);
    }
  };

  Rc<MtlPresenter> createMtlPresenterImpl(
    const Rc<MtlDevice>& device, intptr_t hwnd, const MtlPresenterDesc& desc) {
    return new MtlPresenterImpl(device, hwnd, desc);
  }


}

namespace dxvk {
  Rc<MtlPresenter> backendCreatePresenter(
    const Rc<MtlDevice>& device, intptr_t hwnd, const MtlPresenterDesc& desc) {
    return backend::createMtlPresenterImpl(device, hwnd, desc);
  }
}
