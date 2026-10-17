// Native unit test for the attachment load/store decisions (no Wine, no Metal):
//   clang++ -std=c++17 -I driver/backend test/native/attachment_opt_test.cpp -o /tmp/aot && /tmp/aot
#include "mtl_attachment_opt.h"

#include <cstdio>

using namespace dxvk::backend::attopt;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static SlotInput in(Content c) {
  SlotInput i;
  i.content  = c;
  i.eligible = true;
  return i;
}

// One aspect through a pass, as mtl_context_record.cpp applies it.
struct Sim {
  AspectState st;
  int restores = 0;

  SlotDecision pass(SlotInput i, const Value* clear = nullptr, bool writtenLater = false) {
    i.content = st.content;
    i.explicitClear = clear != nullptr;
    SlotDecision d = decide(i);
    if (d.restoreFirst) {
      restores++;
      st.content = Content::Cleared;
      i.content = st.content;
      d = decide(i);
    }
    i.written = i.written || writtenLater;
    After a = stateAfter(i, d);
    st.content = a.content;
    if (a.passValue)
      st.value = *clear;
    return d;
  }

  // A reader outside a pass (sampling, copy, readback, present), as
  // MtlContextImpl::attRead applies it.
  void read(uint32_t frame = 1u) {
    if (noteRead(st, frame)) {
      restores++;
      st.content = Content::Cleared;
    }
  }

  // A pass in frame `frame`, with the read hint as createPassEncoder sets it.
  SlotDecision passAt(uint32_t frame, SlotInput i, const Value* clear = nullptr) {
    i.readHint = readRecently(st, frame);
    return pass(i, clear);
  }
};

int main() {
  // Defined contents are always loaded and stored.
  {
    SlotDecision d = decide(in(Content::Defined));
    CHECK(d.load == Load::Load && d.store == Store::Store && !d.restoreFirst);
    CHECK(stateAfter(in(Content::Defined), d).content == Content::Defined);
    CHECK(bytesSaved(in(Content::Defined), d, 100u, false) == 0);
  }

  // A known clear nothing writes or samples: Clear-load it, skip the store.
  {
    SlotInput i = in(Content::Cleared);
    SlotDecision d = decide(i);
    CHECK(d.load == Load::Clear && d.loadFromState && d.store == Store::DontCare);
    After a = stateAfter(i, d);
    CHECK(a.content == Content::Lazy && !a.passValue);
    CHECK(bytesSaved(i, d, 100u, false) == 200);
    CHECK(!slotNeedsPass(i, d));
  }

  // Lazy: always Clear-loaded, even with the live flag off.
  {
    SlotInput i = in(Content::Lazy);
    i.optOn = false;
    SlotDecision d = decide(i);
    CHECK(d.load == Load::Clear && d.loadFromState && d.store == Store::Store);
    CHECK(stateAfter(i, d).content == Content::Cleared);
  }

  // Live flag off: a Cleared aspect gets the previous Load/Store.
  {
    SlotInput i = in(Content::Cleared);
    i.optOn = false;
    SlotDecision d = decide(i);
    CHECK(d.load == Load::Load && d.store == Store::Store);
    CHECK(stateAfter(i, d).content == Content::Cleared);
  }

  // Written: stored, Defined afterwards.
  {
    SlotInput i = in(Content::Lazy);
    i.written = true;
    SlotDecision d = decide(i);
    CHECK(d.load == Load::Clear && d.store == Store::Store);
    CHECK(stateAfter(i, d).content == Content::Defined);
  }

  // Sampled during the pass: stored.
  {
    SlotInput i = in(Content::Cleared);
    i.sampled = true;
    SlotDecision d = decide(i);
    CHECK(d.store == Store::Store);
    CHECK(stateAfter(i, d).content == Content::Cleared);
  }

  // Decided early (commands encoded before the pass ends): never skip a store.
  {
    SlotInput i = in(Content::Cleared);
    i.fullKnowledge = false;
    SlotDecision d = decide(i);
    CHECK(d.load == Load::Clear && d.store == Store::Store);
    CHECK(stateAfter(i, d).content == Content::Cleared);
    i.written = true;  // a later draw of the same pass
    CHECK(stateAfter(i, d).content == Content::Defined);
  }

  // An explicit clear nothing draws into: no encoder needed, the value is kept.
  {
    SlotInput i = in(Content::Defined);
    i.explicitClear = true;
    SlotDecision d = decide(i);
    CHECK(d.load == Load::Clear && !d.loadFromState && d.store == Store::DontCare);
    After a = stateAfter(i, d);
    CHECK(a.content == Content::Lazy && a.passValue);
    CHECK(!slotNeedsPass(i, d));
    CHECK(bytesSaved(i, d, 100u, true) == 100);
    i.sampled = true;
    d = decide(i);
    CHECK(d.store == Store::Store && slotNeedsPass(i, d));
    CHECK(stateAfter(i, d).content == Content::Cleared);
  }

  // Undefined (discarded): nothing loaded; nothing stored unless written.
  {
    SlotInput i = in(Content::Undefined);
    SlotDecision d = decide(i);
    CHECK(d.load == Load::DontCare && d.store == Store::DontCare);
    CHECK(stateAfter(i, d).content == Content::Undefined);
    i.written = true;
    d = decide(i);
    CHECK(d.load == Load::DontCare && d.store == Store::Store);
    CHECK(stateAfter(i, d).content == Content::Defined);
    i.optOn = false;
    d = decide(i);
    CHECK(d.load == Load::Load && d.store == Store::Store);
  }

  // Discarded right after the pass (auto depth-stencil at Present).
  {
    SlotInput i = in(Content::Defined);
    i.written = true;
    i.discardAfter = true;
    SlotDecision d = decide(i);
    CHECK(d.load == Load::Load && d.store == Store::DontCare);
    CHECK(stateAfter(i, d).content == Content::Undefined);
    i.fullKnowledge = false;
    d = decide(i);
    CHECK(d.store == Store::Store);
    CHECK(stateAfter(i, d).content == Content::Undefined);
    i.fullKnowledge = true;
    i.optOn = false;
    CHECK(decide(i).store == Store::Store);
  }

  // Ineligible views: the previous behaviour; Lazy contents restored first.
  {
    SlotInput i = in(Content::Lazy);
    i.eligible = false;
    SlotDecision d = decide(i);
    CHECK(d.restoreFirst && d.load == Load::Load && d.store == Store::Store);
    i.content = Content::Cleared;
    d = decide(i);
    CHECK(!d.restoreFirst && d.load == Load::Load && d.store == Store::Store);
    CHECK(stateAfter(i, d).content == Content::Cleared);
    i.explicitClear = true;
    d = decide(i);
    CHECK(d.load == Load::Clear && d.store == Store::Store);
    CHECK(stateAfter(i, d).content == Content::Defined);
    CHECK(slotNeedsPass(i, d));
    i.content = Content::Lazy;
    CHECK(!decide(i).restoreFirst);  // overwritten by the clear anyway
  }

  // SWTOR's shadow color target: created (initImage clear), bound in every
  // shadow pass with color writes off, never read. One store-free pass after
  // another, and the zero contents survive a later read.
  {
    Sim s;
    s.st.content = Content::Cleared;  // initImage: cleared to 0
    for (int pass = 0; pass < 6; pass++) {
      SlotDecision d = s.pass(in(Content::Defined));
      CHECK(d.load == Load::Clear && d.loadFromState && d.store == Store::DontCare);
      CHECK(s.st.content == Content::Lazy);
    }
    CHECK(s.restores == 0);
    s.read();                         // sampled once after all
    CHECK(s.restores == 1 && s.st.content == Content::Cleared);
    SlotDecision d = s.pass(in(Content::Defined));
    CHECK(d.store == Store::DontCare && s.st.content == Content::Lazy);
  }

  // Clear, then render: the clear is folded into the rendering pass's load.
  {
    Sim s;
    const Value v = { { 0.25f, 0.5f, 0.75f, 1.0f }, 0u };
    s.pass(in(Content::Defined), &v);  // a clear-only pass: skipped
    CHECK(s.st.content == Content::Lazy && s.st.value.c[2] == 0.75f);
    SlotInput i = in(Content::Defined);
    i.written = true;
    SlotDecision d = s.pass(i);
    CHECK(d.load == Load::Clear && d.loadFromState && d.store == Store::Store);
    CHECK(s.st.content == Content::Defined && s.restores == 0);
  }

  // The read hint keeps the store of a known clear; it doesn't touch loads,
  // written, Defined or Undefined contents.
  {
    SlotInput i = in(Content::Cleared);
    i.readHint = true;
    SlotDecision d = decide(i);
    CHECK(d.load == Load::Clear && d.loadFromState && d.store == Store::Store);
    CHECK(stateAfter(i, d).content == Content::Cleared);
    CHECK(bytesSaved(i, d, 100u, false) == 100);
    i.explicitClear = true;
    d = decide(i);
    CHECK(d.store == Store::Store && slotNeedsPass(i, d));
    i = in(Content::Undefined);
    i.readHint = true;
    d = decide(i);
    CHECK(d.load == Load::DontCare && d.store == Store::DontCare);
    i = in(Content::Defined);
    i.readHint = true;
    d = decide(i);
    CHECK(d.load == Load::Load && d.store == Store::Store);
  }

  // noteRead: only known clears are remembered; only Lazy needs a restore.
  {
    AspectState st;
    CHECK(!noteRead(st, 5u) && st.readFrame == 0u);           // Defined
    st.content = Content::Cleared;
    CHECK(!noteRead(st, 5u) && st.readFrame == 5u);
    st.content = Content::Lazy;
    CHECK(noteRead(st, 6u) && st.readFrame == 6u);
    st.content = Content::Undefined;
    CHECK(!noteRead(st, 9u) && st.readFrame == 6u);
    CHECK(readRecently(st, 6u) && readRecently(st, 6u + kReadHintFrames - 1u));
    CHECK(!readRecently(st, 6u + kReadHintFrames));
    CHECK(!readRecently(AspectState(), 1u));
  }

  // A target cleared every frame, never drawn into, sampled by a later pass
  // every frame: re-cleared for its reader in the first frame only; from then
  // on its pass stores the clear (no pass of its own, nothing re-cleared).
  // When the reads stop, the hint lapses and the store is skipped again.
  {
    Sim s;
    const Value v = { { 0.0f, 0.8f, 0.0f, 1.0f }, 0u };
    for (uint32_t f = 1u; f <= 20u; f++) {
      SlotDecision d = s.passAt(f, in(Content::Defined), &v);
      CHECK(d.load == Load::Clear && !d.loadFromState);
      CHECK(d.store == (f == 1u ? Store::DontCare : Store::Store));
      s.read(f);
      CHECK(s.st.content == Content::Cleared && s.st.value.c[1] == 0.8f);
    }
    CHECK(s.restores == 1);
    for (uint32_t f = 21u; f <= 40u; f++) {
      SlotDecision d = s.passAt(f, in(Content::Defined), &v);
      CHECK(d.store == (f < 20u + kReadHintFrames ? Store::Store : Store::DontCare));
    }
    CHECK(s.st.content == Content::Lazy && s.restores == 1);
    s.read(41u);                                               // reads again: one restore, hint back
    CHECK(s.restores == 2);
    CHECK(s.passAt(42u, in(Content::Defined), &v).store == Store::Store);
  }

  // SWTOR's shadow atlas (D24S8): depth and stencil cleared, depth written,
  // stencil never; then sampled as a shadow map, which reads depth only. The
  // stencil isn't a reader's concern: it stays Lazy, no restore, no hint, and
  // the next pass on the atlas Clear-loads it.
  {
    Sim depth, stencil;
    const Value z = { { 1.0f, 0.0f, 0.0f, 0.0f }, 0u }, s0 = { { }, 0u };
    for (uint32_t f = 1u; f <= 10u; f++) {
      SlotInput w = in(Content::Defined);
      w.written = true;
      depth.passAt(f, w, &z);
      SlotDecision ds = stencil.passAt(f, in(Content::Defined), &s0);
      CHECK(ds.load == Load::Clear && ds.store == Store::DontCare);
      depth.read(f);                                           // the sampling bind: depth aspect only
      CHECK(depth.st.content == Content::Defined && stencil.st.content == Content::Lazy);
    }
    CHECK(depth.restores == 0 && stencil.restores == 0);
  }

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("attachment_opt_test: OK\n");
  return 0;
}
