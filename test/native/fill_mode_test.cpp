// Native unit test for D3DRS_FILLMODE on the render encoder (driver/backend/mtl_fill_mode.h, #161)
// and MtlContextImpl::flushDynamicState's use of it, modelled here exactly: every encoder start
// resets m_dynValid (so the next flush is 'first'), each flush sends the fill mode when
// fillModeNeedsCommand says so and then records it in m_lastFillLines. A new Metal encoder starts
// in Fill; with deferred passes an encoder may be merged with the one before and keep its state.
//  A. solid only: no command at all without deferred passes (no cost for the game);
//  B. wireframe draws: the encoder is in Lines exactly for them, back in Fill after, over
//     several encoders, a fresh encoder after a Lines draw included;
//  C. deferred passes: an encoder merged with one left in Lines still draws solid when asked;
//  D. a run of the same mode sends one command, not one per flush.
// Built with -DFILL_MODE_TEST_OLD, no fill mode is ever sent (the code before #161): B and C fail
// there, which is what this change fixes.
//   clang++ -std=c++17 -I driver/backend test/native/fill_mode_test.cpp -o /tmp/fmt && /tmp/fmt
#include "mtl_fill_mode.h"

#include <cstdio>
#include <initializer_list>

using namespace dxvk::backend;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

struct Context {
  bool     deferred;
  bool     encoderLines   = false;  // the Metal encoder's actual fill mode
  bool     dynValid       = false;  // m_dynValid
  bool     lastFillLines  = false;  // m_lastFillLines
  unsigned commands       = 0u;

  explicit Context(bool d) : deferred(d) { }

  // materializePass: m_dynValid = false; a new encoder is in Fill, a merged one keeps its state
  void newEncoder(bool merged = false) {
    dynValid = false;
    if (!merged)
      encoderLines = false;
  }

  // a draw: flushDynamicState (DirtyEverything after an encoder start, DirtyRasterizer on a
  // change), then whether the draw sees the fill mode it asked for
  bool draw(bool lines) {
    const bool first = !dynValid;
    dynValid = true;
#ifndef FILL_MODE_TEST_OLD
    if (fillModeNeedsCommand(first, lines, lastFillLines, deferred)) {
      encoderLines = lines;
      commands++;
    }
    lastFillLines = lines;
#endif
    return encoderLines == lines;
  }
};

int main() {
  // A
  {
    Context c(false);
    for (int e = 0; e < 5; e++) {
      c.newEncoder();
      for (int d = 0; d < 10; d++)
        CHECK(c.draw(false));
    }
    CHECK(c.commands == 0u);
  }
  // B
  {
    Context c(false);
    c.newEncoder();
    CHECK(c.draw(false));
    CHECK(c.draw(true));
    CHECK(c.draw(true));
    CHECK(c.draw(false));
    c.newEncoder();
    CHECK(c.draw(true));
    c.newEncoder();             // a fresh encoder after a Lines draw: Fill again, and told so
    CHECK(c.draw(true));
    CHECK(c.draw(false));
    c.newEncoder();
    CHECK(c.draw(false));
  }
  // C
  {
    Context c(true);
    c.newEncoder();
    CHECK(c.draw(true));
    c.newEncoder(true);         // merged: still in Lines
    CHECK(c.draw(false));
    c.newEncoder(true);
    CHECK(c.draw(true));
    c.newEncoder();
    CHECK(c.draw(false));
    for (bool merged : { false, true, true, false }) {
      c.newEncoder(merged);
      CHECK(c.draw(true));
      CHECK(c.draw(false));
    }
  }
  // D
  {
    Context c(false);
    c.newEncoder();
    for (int d = 0; d < 20; d++)
      CHECK(c.draw(true));
    CHECK(c.commands == 1u);
  }

  if (g_fail == 0)
    std::printf("fill_mode_test: ok\n");
  return g_fail == 0 ? 0 : 1;
}
