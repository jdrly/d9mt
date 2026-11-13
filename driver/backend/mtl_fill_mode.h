#pragma once

namespace dxvk::backend {

  /**
   * \brief Whether a dynamic-state flush sends the triangle fill mode (D3DRS_FILLMODE, #161)
   *
   * A new render encoder starts in Fill, so on its first flush ('first': m_dynValid was reset
   * when materializePass started the encoder) only Lines needs a command. Deferred passes may
   * merge encoders, so a "first" flush can find the state an earlier pass left: with them on,
   * the first flush always states it (the depth clip mode's rule). After that, only a change
   * needs a command.
   *
   * \param [in] first The encoder's first dynamic-state flush
   * \param [in] lines The draw wants MTLTriangleFillModeLines
   * \param [in] lastLines What the encoder was last told (ignored when \p first)
   * \param [in] deferredPasses D9MT_DEFERRED is on
   */
  inline bool fillModeNeedsCommand(bool first, bool lines, bool lastLines, bool deferredPasses) {
    return first ? (lines || deferredPasses) : lines != lastLines;
  }

}
