#pragma once

// Attachment load/store optimization: which load and store action each
// attachment of a render pass gets, and what the attachment holds afterwards.
//
// The backend tracks, per single-subresource attachment image and per aspect
// (color, depth, stencil), what it can prove about the contents:
//
//   Defined    memory holds the contents (values unknown): Load and Store.
//   Cleared    memory holds a full clear to a known value (initImage, or a
//              clear that was stored): a pass can Clear-load it instead of
//              reading memory, and needs no store if nothing writes it.
//   Lazy       the contents are a full clear to a known value, but memory
//              does not hold it (the store was skipped). Every pass that uses
//              it Clear-loads the value; every other reader (sampling,
//              copies, blits, resolves, readbacks, present) re-clears memory
//              first. So contents a later reader needs are never lost.
//   Undefined  the contents are undefined (D3D9 discard of the auto
//              depth-stencil at Present): no load, and nothing to keep.
//
// A store is skipped only when the attachment is not written in the pass,
// nothing samples it during the pass, no reader needed its known clear in
// memory in the last kReadHintFrames frames (readHint), and its contents are
// a known clear (Lazy afterwards) or undefined. Contents that only memory
// holds (Defined) are always loaded and stored.
//
// The read hint: a target cleared every frame and then read outside a pass
// every frame (sampled by a later pass, copied, read back) would otherwise be
// left Lazy and re-cleared for its reader every frame, a pass of its own.
// Once a reader has needed such a clear, the passes of the next frames store
// it as before; the hint lapses when the reads stop.
//
// Header-only and dependency-free so the decision table can be unit-tested
// natively (test/native/attachment_opt_test.cpp); mtl_context_record.cpp
// applies it.

#include <cstdint>

namespace dxvk::backend::attopt {

  enum class Content : uint8_t { Defined, Cleared, Lazy, Undefined };

  enum class Load  : uint8_t { Load, Clear, DontCare };
  enum class Store : uint8_t { Store, DontCare };

  // A clear value: color (rgba), or depth in c[0], or stencil.
  struct Value {
    float    c[4]    = { };
    uint32_t stencil = 0u;
  };

  // Frames a read of a known clear keeps the aspect's stores (readHint).
  constexpr uint32_t kReadHintFrames = 8u;

  struct AspectState {
    Content  content   = Content::Defined;
    Value    value     = { };   // Cleared / Lazy
    uint32_t readFrame = 0u;    // frame (1-based) a reader last needed its known clear in memory; 0: never
  };

  // A reader outside a render pass (sampling bind, copy, blit, resolve,
  // readback, present, partial write) needs memory to hold this aspect.
  // Remembers a known clear being read (the read hint) and returns whether
  // the aspect must be restored first (Lazy). `frame` is 1-based.
  inline bool noteRead(AspectState& st, uint32_t frame) {
    if (st.content == Content::Lazy || st.content == Content::Cleared)
      st.readFrame = frame;
    return st.content == Content::Lazy;
  }

  // A reader needed this aspect's known clear in one of the last
  // kReadHintFrames frames.
  inline bool readRecently(const AspectState& st, uint32_t frame) {
    return st.readFrame != 0u && frame - st.readFrame < kReadHintFrames;
  }

  struct SlotInput {
    Content content       = Content::Defined;  // before the pass
    bool    explicitClear = false;  // the pass clears this aspect (a pending clear folded into the load)
    bool    eligible      = false;  // single-subresource 2D image, attachment view in the image's format
    bool    optOn         = true;   // false: live flag no_attach_opt (only what correctness needs)
    bool    fullKnowledge = true;   // decided at the end of the pass: `written` and `sampled` are final
    bool    written       = false;  // a draw in the pass may write this aspect
    bool    sampled       = false;  // bound for sampling during the pass, or still bound
    bool    readHint      = false;  // readRecently(): a reader needed this known clear lately
    bool    discardAfter  = false;  // the contents are discarded right after the pass (D3D9 discard)
  };

  struct SlotDecision {
    Load  load           = Load::Load;
    bool  loadFromState  = false;  // a Clear load of the state's value, not of the pass's clear
    Store store          = Store::Store;
    bool  restoreFirst   = false;  // Lazy contents used through an ineligible view: re-clear memory first
  };

  // Load and store actions. Callers must restore first when `restoreFirst`
  // is set and then decide again with content = Cleared.
  inline SlotDecision decide(const SlotInput& in) {
    SlotDecision d;

    if (!in.eligible) {
      // The previous behaviour: Load unless the pass clears, always Store.
      d.load = in.explicitClear ? Load::Clear : Load::Load;
      d.restoreFirst = in.content == Content::Lazy && !in.explicitClear;
      return d;
    }

    if (in.explicitClear) {
      d.load = Load::Clear;
    } else {
      switch (in.content) {
        case Content::Lazy:
          // Memory doesn't hold the contents: the Clear load is what keeps
          // them, whatever the live flag says.
          d.load = Load::Clear;
          d.loadFromState = true;
          break;
        case Content::Cleared:
          if (in.optOn) {
            d.load = Load::Clear;
            d.loadFromState = true;
          }
          break;
        case Content::Undefined:
          if (in.optOn)
            d.load = Load::DontCare;
          break;
        case Content::Defined:
          break;
      }
    }

    if (in.optOn && in.fullKnowledge) {
      if (in.discardAfter) {
        d.store = Store::DontCare;
      } else if (!in.written && !in.sampled
              && (d.load == Load::Clear ? !in.readHint : in.content == Content::Undefined)) {
        // Nothing changes the contents, they are a known clear (or
        // undefined) and no reader wanted them lately: memory needn't hold
        // them.
        d.store = Store::DontCare;
      }
    }
    return d;
  }

  struct After {
    Content content   = Content::Defined;
    bool    passValue = false;  // Cleared/Lazy: the value is the pass's clear (else the state's)
  };

  // What the aspect holds after the pass. `written`/`discardAfter` are the
  // final values for the whole pass.
  inline After stateAfter(const SlotInput& in, const SlotDecision& d) {
    After a;
    if (in.discardAfter) {
      a.content = Content::Undefined;
      return a;
    }
    if (in.written) {
      a.content = Content::Defined;
      return a;
    }
    if (!in.eligible) {
      // An ineligible view: its clear value isn't recorded in the image's
      // format. Unwritten and unclear, memory still holds what it held
      // (a Lazy state was restored to Cleared before the pass).
      a.content = in.explicitClear ? Content::Defined
                : in.content == Content::Lazy ? Content::Cleared
                : in.content;
      return a;
    }
    if (d.load == Load::Clear) {
      a.content   = d.store == Store::DontCare ? Content::Lazy : Content::Cleared;
      a.passValue = !d.loadFromState;
      return a;
    }
    if (d.load == Load::DontCare || in.content == Content::Undefined) {
      a.content = Content::Undefined;
      return a;
    }
    // Loaded and stored unchanged.
    a.content = in.content == Content::Lazy ? Content::Cleared : in.content;
    return a;
  }

  // A pass without draws (no resolve folded into it) is a no-op, and needs
  // no encoder, unless it stores an explicit clear.
  inline bool slotNeedsPass(const SlotInput& in, const SlotDecision& d) {
    return in.explicitClear && d.store == Store::Store;
  }

  // Bytes moved by a load and a store of an attachment, against the previous
  // behaviour (Load unless the pass clears, always Store): the estimate the
  // STATS line reports as saved.
  inline int64_t bytesSaved(const SlotInput& in, const SlotDecision& d, uint64_t bytes, bool passSkipped) {
    const int64_t b = int64_t(bytes);
    int64_t oldCost = (in.explicitClear ? 0 : b) + b;
    int64_t newCost = 0;
    if (!passSkipped) {
      newCost += d.load == Load::Load ? b : 0;
      newCost += d.store == Store::Store ? b : 0;
    }
    return oldCost - newCost;
  }

}
