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

  // A reader outside a pass (sampling, copy, readback, present).
  void read() {
    if (st.content == Content::Lazy) {
      restores++;
      st.content = Content::Cleared;
    }
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

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("attachment_opt_test: OK\n");
  return 0;
}
