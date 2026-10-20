#pragma once

// D9MT_LIFETIME_CHECK: a debug check of the Metal 4 lifetime rule. Command buffers don't
// retain what they use, so a texture d9mt releases while a recorded or committed command
// buffer still refers to it is a use-after-free: the GPU reads a wrong texture or faults
// (kIOGPUCommandBufferCallbackErrorPageFault, the pink Cartel Market and the freeze of #23).
// Whether the GPU then faults depends on the machine and on timing; this check doesn't:
// every texture handle put into a command buffer (a sampled view's resource ID in an
// argument table, a pass attachment, a copy source or destination) is stamped with that
// command buffer's serial, and releasing d9mt's last reference to it before the GPU is past
// that serial is reported, on any Mac and on the virtual display alike.
//   D9MT_LIFETIME_CHECK=1 (or abort): log "D9MT-LIFETIME ..." and abort at the first one
//   D9MT_LIFETIME_CHECK=log: log them (the first 100, then a count every 1000) and go on
// A drawable's texture is also held by Core Animation until it is on screen, so releasing
// d9mt's reference early frees nothing: those are logged as "D9MT-LIFETIME drawable"
// (rate-limited) and never abort, to show the present path's assumptions.
// Off by default: one predictable branch per site.

#include <cstdint>

#include "../../bridge/winemetal.h"
#include "../util/util_likely.h"

namespace dxvk::backend {

class SubmissionTracker;

namespace lifetime {

  enum class Use : uint8_t { Sampled, Attachment, CopySrc, CopyDst };

  // 0 off, 1 abort, 2 log (D9MT_LIFETIME_CHECK, read once)
  int mode();

  inline bool enabled() { return mode() != 0; }

  // The tracker whose serials the stamps are (the device's); nullptr when it goes.
  void attach(SubmissionTracker* tracker);
  void detach(SubmissionTracker* tracker);

  void noteAcquireSlow(obj_handle_t texture);
  void noteUseSlow(obj_handle_t texture, uint64_t serial, Use use);
  void markDrawableSlow(obj_handle_t texture);
  void checkReleaseSlow(obj_handle_t texture, const char* what);

  // d9mt takes a reference to texture (WmtRef<Texture>::adopt/retain): only the release of the
  // last one is checked, as several wrappers can hold the same texture (a drawable's, which
  // Core Animation hands out again while an older frame's wrapper still holds it)
  inline void noteAcquire(obj_handle_t texture) {
    if (unlikely(enabled()) && texture)
      noteAcquireSlow(texture);
  }

  // texture is used by the command buffer of serial
  inline void noteUse(obj_handle_t texture, uint64_t serial, Use use) {
    if (unlikely(enabled()) && texture)
      noteUseSlow(texture, serial, use);
  }

  // texture is a drawable's (Core Animation holds it too)
  inline void markDrawable(obj_handle_t texture) {
    if (unlikely(enabled()) && texture)
      markDrawableSlow(texture);
  }

  // d9mt is about to release a reference to texture (checked if it is the last one)
  inline void checkRelease(obj_handle_t texture, const char* what) {
    if (unlikely(enabled()) && texture)
      checkReleaseSlow(texture, what);
  }

  // submit: the serial the command buffer recorded as recorded got (they must match, or the
  // stamps are off; logged once)
  void checkSubmitSerial(uint64_t recorded, uint64_t submitted);

}

}
