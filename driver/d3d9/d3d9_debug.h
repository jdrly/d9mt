#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../util/util_d9mt_debug.h"

namespace dxvk {

  /* Live debug facility (all on the API thread, under the device lock).
   * Strictly opt-in: with neither env var set nothing is polled or written.
   *  - D9MT_DEBUG_FILE=<Windows path>: flags file polled at most once a
   *    second on Present, one flag per line, '#' comments;
   *  - `dump` dumps the next full frame (once per distinct file mtime) to
   *    d9mt-dump-<n>.txt next to the flags file, shader disassembly to
   *    d9mt-shaders\<name>.txt;
   *  - occ_trace / flag changes go to d9mt-debug.log next to the flags file
   *    (DXVK_LOG_LEVEL=warn would drop info lines);
   *  - D9MT_SHADER_DUMP_PATH=<Windows dir>: disassembly of vertex shaders
   *    rejected (or accepted via d3d9.allowOutOfRangeVsDefs) for using more
   *    than 256 float constants; defaults to d9mt-shaders\ next to the flags
   *    file when only D9MT_DEBUG_FILE is set. */
  struct D3D9DebugState {
    // Flags file
    std::string flagsPath;     // empty: flags file disabled
    std::string dir;
    std::string shaderDir;     // empty: shader text dumps disabled
    bool        pathResolved  = false;
    bool        pathInit      = false;
    uint64_t    lastPollMs    = 0u;
    bool        fileSeen      = false;
    uint64_t    fileMtime     = 0u;
    uint64_t    fileSize      = 0u;
    uint64_t    dumpMtime     = 0u;
    uint32_t    flags         = 0u;

    // Hooks are only taken when this is set (dumping or occ_trace).
    bool        hooks         = false;

    // Frame / draw counters (frame-local draw index).
    uint64_t    frame         = 0u;
    uint32_t    drawIdx       = 0u;

    // Frame dump
    bool        dumpArmed     = false;
    bool        dumping       = false;
    std::FILE*  dumpFile      = nullptr;
    uint32_t    dumpCounter   = 0u;
    std::unordered_map<const void*, uint32_t> objIds;
    std::unordered_set<std::string>           shadersWritten;

    // Debug log (occ_trace + flag changes), rate-limited per second.
    std::FILE*  logFile       = nullptr;
    uint32_t    logBudget     = 0u;
    uint64_t    logSuppressed = 0u;

    // Begun occlusion queries (by debug id, in Begin order); only tracked
    // while hooks are on.
    struct ActiveQuery {
      uint32_t id;
      uint32_t draw;
      uint64_t frame;
    };
    std::vector<ActiveQuery> activeOcc;
  };

}
