#include "d3d9_device.h"
#include "d3d9_query.h"
#include "d3d9_shader.h"
#include "d3d9_surface.h"
#include "d3d9_texture.h"
#include "d3d9_names.h"

#include "../util/util_env.h"
#include "../util/util_string.h"
#include "../util/log/log.h"

#include <util/util_byte_stream.h>
#include <sm3/sm3_parser.h>
#include <sm3/sm3_disasm.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace dxvk {

  namespace {

    struct FlagName {
      const char* name;
      uint32_t    bit;
    };

    constexpr FlagName g_flagNames[] = {
      { "occ_visible",      d9mtdbg::OccVisible     },
      { "occ_trace",        d9mtdbg::OccTrace       },
      { "occ_sync",         d9mtdbg::OccSync        },
      { "no_scissor",       d9mtdbg::NoScissor      },
      { "no_depthbias",     d9mtdbg::NoDepthBias    },
      { "cull_none",        d9mtdbg::CullNone       },
      { "depth_clamp",      d9mtdbg::DepthClamp     },
      { "no_ztest_blended", d9mtdbg::NoZTestBlended },
      { "no_sameview_skip", d9mtdbg::NoSameViewSkip },
      { "no_clipplanes",    d9mtdbg::NoClipPlanes   },
      { "bools_off",        d9mtdbg::BoolsOff       },
    };

    constexpr uint32_t kLogLinesPerSecond = 400u;

    std::string flagsToString(uint32_t flags) {
      std::string s;
      for (const auto& f : g_flagNames) {
        if (flags & f.bit) {
          if (!s.empty()) s += ' ';
          s += f.name;
        }
      }
      return s.empty() ? std::string("(none)") : s;
    }

    const char* cmpName(DWORD v) {
      static const char* n[] = { "?", "NV", "LT", "EQ", "LE", "GT", "NE", "GE", "AL" };
      return v <= 8u ? n[v] : "?";
    }

    const char* blendName(DWORD v) {
      static const char* n[] = { "?", "ZERO", "ONE", "SC", "ISC", "SA", "ISA", "DA", "IDA",
        "DC", "IDC", "SAS", "BSA", "BISA", "BF", "IBF", "SC1", "ISC1" };
      return v <= 17u ? n[v] : "?";
    }

    const char* blendOpName(DWORD v) {
      static const char* n[] = { "?", "ADD", "SUB", "RSUB", "MIN", "MAX" };
      return v <= 5u ? n[v] : "?";
    }

    const char* cullName(DWORD v) {
      static const char* n[] = { "?", "NONE", "CW", "CCW" };
      return v <= 3u ? n[v] : "?";
    }

    const char* stencilOpName(DWORD v) {
      static const char* n[] = { "?", "KEEP", "ZERO", "REPL", "INCS", "DECS", "INV", "INC", "DEC" };
      return v <= 8u ? n[v] : "?";
    }

    const char* primName(D3DPRIMITIVETYPE t) {
      static const char* n[] = { "?", "PL", "LL", "LS", "TL", "TS", "TF" };
      return uint32_t(t) <= 6u ? n[uint32_t(t)] : "?";
    }

    std::string fmtName(D3D9Format f) {
      std::ostringstream ss;
      ss << f;
      std::string s = ss.str();
      size_t p = s.rfind("::");
      return p != std::string::npos ? s.substr(p + 2u) : s;
    }

    std::string hrName(HRESULT hr) {
      switch (hr) {
        case S_OK:               return "S_OK";
        case S_FALSE:            return "S_FALSE";
        case D3DERR_INVALIDCALL: return "INVALIDCALL";
        case D3DERR_DEVICELOST:  return "DEVICELOST";
        default:                 return str::format("0x", std::hex, uint32_t(hr));
      }
    }

    bool fileExists(const std::string& path) {
      return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

  }


  uint32_t D3D9DeviceEx::DebugNewQueryId() {
    static std::atomic<uint32_t> s_next = { 1u };
    return s_next.fetch_add(1u);
  }


  void D3D9DeviceEx::DebugInitPath() {
    if (m_dbg.pathResolved)
      return;
    m_dbg.pathResolved = true;

    // Opt-in only: no default path, nothing is polled or written unless set.
    std::string path = env::getEnvVar("D9MT_DEBUG_FILE");
    if (!path.empty()) {
      m_dbg.flagsPath = path;
      size_t sep = path.find_last_of("\\/");
      m_dbg.dir = sep != std::string::npos ? path.substr(0u, sep) : std::string(".");
      Logger::warn(str::format("D9MT-DEBUG flags file: ", path));
    }

    std::string shaders = env::getEnvVar("D9MT_SHADER_DUMP_PATH");
    while (!shaders.empty() && (shaders.back() == '\\' || shaders.back() == '/'))
      shaders.pop_back();
    if (!shaders.empty())
      m_dbg.shaderDir = shaders;
    else if (!m_dbg.dir.empty())
      m_dbg.shaderDir = m_dbg.dir + "\\d9mt-shaders";
  }


  void D3D9DeviceEx::DebugLogRaw(const std::string& line) {
    if (m_dbg.dir.empty())
      return;
    if (!m_dbg.logFile) {
      m_dbg.logFile = std::fopen((m_dbg.dir + "\\d9mt-debug.log").c_str(), "a");
      if (!m_dbg.logFile)
        return;
    }
    std::fputs(line.c_str(), m_dbg.logFile);
    std::fputc('\n', m_dbg.logFile);
  }


  void D3D9DeviceEx::DebugLog(const std::string& line) {
    if (!m_dbg.logBudget) {
      m_dbg.logSuppressed++;
      return;
    }
    m_dbg.logBudget--;
    DebugLogRaw(line);
  }


  void D3D9DeviceEx::DebugUpdateHooks() {
    m_dbg.hooks = m_dbg.dumping || (m_dbg.flags & d9mtdbg::OccTrace);
    if (!m_dbg.hooks)
      m_dbg.activeOcc.clear();
  }


  void D3D9DeviceEx::DebugSetFlags(uint32_t flags) {
    if (flags == m_dbg.flags)
      return;

    m_dbg.flags = flags;
    d9mtdbg::g_flags.store(flags, std::memory_order_relaxed);

    std::string msg = str::format("D9MT-DEBUG flags: ", flagsToString(flags), " (frame ", m_dbg.frame, ")");
    Logger::warn(msg);
    DebugLogRaw(msg);
    if (m_dbg.logFile)
      std::fflush(m_dbg.logFile);

    // Re-bind every state a toggle rewrites.
    m_dirty.set(D3D9DeviceDirtyFlag::ViewportScissor,
                D3D9DeviceDirtyFlag::RasterizerState,
                D3D9DeviceDirtyFlag::DepthBias,
                D3D9DeviceDirtyFlag::DepthStencilState,
                D3D9DeviceDirtyFlag::ClipPlanes);
  }


  void D3D9DeviceEx::DebugPoll() {
    const bool startup = !m_dbg.pathInit;
    m_dbg.pathInit = true;
    DebugInitPath();
    if (m_dbg.flagsPath.empty())
      return;

    if (m_dbg.logSuppressed) {
      DebugLogRaw(str::format("D9MT-DEBUG log rate limit: ", m_dbg.logSuppressed, " lines suppressed"));
      m_dbg.logSuppressed = 0u;
    }
    m_dbg.logBudget = kLogLinesPerSecond;
    if (m_dbg.logFile)
      std::fflush(m_dbg.logFile);

    WIN32_FILE_ATTRIBUTE_DATA fad = { };
    if (!GetFileAttributesExA(m_dbg.flagsPath.c_str(), GetFileExInfoStandard, &fad)
     || (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      if (m_dbg.fileSeen) {
        m_dbg.fileSeen = false;
        DebugSetFlags(0u);
        DebugUpdateHooks();
      }
      return;
    }

    const uint64_t mtime = (uint64_t(fad.ftLastWriteTime.dwHighDateTime) << 32)
                         | uint64_t(fad.ftLastWriteTime.dwLowDateTime);
    const uint64_t size  = (uint64_t(fad.nFileSizeHigh) << 32) | uint64_t(fad.nFileSizeLow);

    if (m_dbg.fileSeen && mtime == m_dbg.fileMtime && size == m_dbg.fileSize)
      return;

    m_dbg.fileSeen  = true;
    m_dbg.fileMtime = mtime;
    m_dbg.fileSize  = size;

    std::string text;
    if (std::FILE* f = std::fopen(m_dbg.flagsPath.c_str(), "rb")) {
      char buf[4096];
      size_t n;
      while ((n = std::fread(buf, 1u, sizeof(buf), f)) > 0u && text.size() < (1u << 16))
        text.append(buf, n);
      std::fclose(f);
    }

    uint32_t flags = 0u;
    bool dump = false;
    std::string unknown;

    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
      size_t hash = line.find('#');
      if (hash != std::string::npos)
        line.resize(hash);
      for (char& c : line) {
        if (c == ',' || c == ';' || c == '\r' || c == '\t')
          c = ' ';
        c = char(std::tolower(uint8_t(c)));
      }
      std::istringstream words(line);
      std::string w;
      while (words >> w) {
        if (w == "dump") {
          dump = true;
          continue;
        }
        bool known = false;
        for (const auto& fn : g_flagNames) {
          if (w == fn.name) {
            flags |= fn.bit;
            known = true;
          }
        }
        if (!known)
          unknown += (unknown.empty() ? "" : " ") + w;
      }
    }

    if (!unknown.empty())
      DebugLogRaw(str::format("D9MT-DEBUG unknown flags ignored: ", unknown));

    DebugSetFlags(flags);

    if (dump && mtime != m_dbg.dumpMtime) {
      m_dbg.dumpMtime = mtime;
      // A `dump` left in the file from an earlier session must not fire
      // during startup; re-saving the file triggers it.
      if (startup) {
        DebugLogRaw("D9MT-DEBUG `dump` present at startup ignored; re-save the file to dump a frame");
      } else if (!m_dbg.dumping) {
        DebugBeginDump();
      }
    }

    DebugUpdateHooks();
    if (m_dbg.logFile)
      std::fflush(m_dbg.logFile);
  }


  void D3D9DeviceEx::DebugOnPresent() {
    if (unlikely(m_dbg.dumping))
      DebugEndDump();

    m_dbg.frame++;
    m_dbg.drawIdx = 0u;

    if (unlikely(m_dbg.activeOcc.size() > 256u))
      m_dbg.activeOcc.clear();

    // Without D9MT_DEBUG_FILE, DebugPoll runs once (resolving the env vars)
    // and Present never stats a file.
    if (likely(m_dbg.pathInit && m_dbg.flagsPath.empty()))
      return;
    const uint64_t now = GetTickCount64();
    if (likely(m_dbg.pathInit && now - m_dbg.lastPollMs < 1000u))
      return;
    m_dbg.lastPollMs = now;
    DebugPoll();
  }


  void D3D9DeviceEx::DebugBeginDump() {
    std::string path;
    uint32_t n = 1u;
    for (; n < 100000u; n++) {
      path = str::format(m_dbg.dir, "\\d9mt-dump-", n, ".txt");
      if (!fileExists(path))
        break;
    }

    m_dbg.dumpFile = std::fopen(path.c_str(), "w");
    if (!m_dbg.dumpFile) {
      DebugLogRaw(str::format("D9MT-DEBUG cannot open dump file ", path));
      return;
    }

    if (!m_dbg.shaderDir.empty())
      CreateDirectoryA(m_dbg.shaderDir.c_str(), nullptr);

    m_dbg.dumping     = true;
    m_dbg.dumpCounter = n;
    m_dbg.objIds.clear();

    std::fprintf(m_dbg.dumpFile,
      "# d9mt frame dump %u: frame %llu, flags: %s\n"
      "# D<n> <api> <prim>x<count> [i<inst>] vs= ps= (shader text: d9mt-shaders\\<name>.txt)\n"
      "#   rtN=#obj:fmt:WxH[:msN] ds=#obj:fmt:WxH:rw|ro|unbound vp=x,y,w,h,minz,maxz\n"
      "#   z=ZENABLE:ZFUNC:wZWRITE cull= bl=0|1:src/dst/op[ a=src/dst/op] cw=COLORWRITE0\n"
      "#   at=0|1[:func:ref] st=0|1[:func:ref:mask:wmask:pass/fail/zfail[:2s]] sc=0|1[:l,t,r,b]\n"
      "#   db=depthbias,slope clip=mask srgb= occ=<active occlusion queries> tex=sN=#obj:fmt:WxH[:shadow][:=DS][:=RTn]\n"
      "# #obj ids are per dump and identify the underlying texture (an RT surface\n"
      "# and the texture sampling it share an id). Events: CLEAR/STRETCH/RESZ/\n"
      "# COLORFILL, Q<id> BEGIN/END/DATA (DATA = what GetData returned to the game).\n",
      n, (unsigned long long)m_dbg.frame, flagsToString(m_dbg.flags).c_str());

    DebugLogRaw(str::format("D9MT-DEBUG dumping frame ", m_dbg.frame, " -> ", path));
  }


  void D3D9DeviceEx::DebugEndDump() {
    if (m_dbg.dumpFile) {
      std::fprintf(m_dbg.dumpFile, "PRESENT frame=%llu draws=%u\n",
        (unsigned long long)m_dbg.frame, m_dbg.drawIdx);
      std::fclose(m_dbg.dumpFile);
      m_dbg.dumpFile = nullptr;
    }
    DebugLogRaw(str::format("D9MT-DEBUG dump ", m_dbg.dumpCounter, " written (",
      m_dbg.drawIdx, " draws)"));
    if (m_dbg.logFile)
      std::fflush(m_dbg.logFile);
    m_dbg.dumping = false;
    DebugUpdateHooks();
  }


  uint32_t D3D9DeviceEx::DebugObjId(const void* p) {
    if (!p)
      return 0u;
    auto it = m_dbg.objIds.emplace(p, uint32_t(m_dbg.objIds.size() + 1u)).first;
    return it->second;
  }


  void D3D9DeviceEx::DebugEvent(const std::string& text) {
    if (!m_dbg.dumpFile)
      return;
    std::fprintf(m_dbg.dumpFile, "  %s @D%u\n", text.c_str(), m_dbg.drawIdx);
  }


  std::string D3D9DeviceEx::DebugSurfaceDesc(D3D9CommonTexture* tex) {
    if (!tex)
      return "null";
    const D3D9_COMMON_TEXTURE_DESC* d = tex->Desc();
    std::string s = str::format("#", DebugObjId(tex), ":", fmtName(d->Format), ":",
      d->Width, "x", d->Height);
    if (d->MultiSample >= D3DMULTISAMPLE_2_SAMPLES)
      s += str::format(":ms", uint32_t(d->MultiSample));
    return s;
  }


  void D3D9DeviceEx::DebugWriteShader(const std::string& name, const void* code, size_t size) {
    if (m_dbg.shaderDir.empty())
      return;
    if (!m_dbg.shadersWritten.insert(name).second)
      return;
    const std::string path = str::format(m_dbg.shaderDir, "\\", name, ".txt");
    if (fileExists(path))
      return;
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
      return;

    std::fprintf(f, "; %s, %zu bytes\n", name.c_str(), size);
    try {
      using namespace dxbc_spv;
      sm3::Parser parser(util::ByteReader(code, size));
      if (parser) {
        const sm3::ShaderInfo info = parser.getShaderInfo();
        std::fprintf(f, "%s_%u_%u\n", info.getType() == sm3::ShaderType::ePixel ? "ps" : "vs",
          uint32_t(info.getVersion().first), uint32_t(info.getVersion().second));
        sm3::Disassembler::Options opt = { };
        opt.indent = true;
        sm3::Disassembler dis(opt, parser.getShaderInfo());
        sm3::ConstantTable ctab;
        while (parser) {
          sm3::Instruction op = parser.parseInstruction();
          if (!op) {
            std::fputs("; <parse error>\n", f);
            break;
          }
          if (op.getOpCode() == sm3::OpCode::eComment) {
            if (!ctab)
              ctab = sm3::ConstantTable(util::ByteReader(op.getCommentData(), op.getCommentDataSize()));
            continue;
          }
          std::string line = dis.disassembleOp(op, ctab);
          std::fputs(line.c_str(), f);
          std::fputc('\n', f);
        }
        if (ctab) {
          std::fputs("; constant table:\n", f);
          for (const auto& c : ctab.entries()) {
            static const char* prefix[] = { "b", "i", "c", "s" };
            const uint32_t set = uint32_t(c.registerSet);
            std::fprintf(f, ";   %s %s%u count=%u\n", c.name.c_str(),
              set < 4u ? prefix[set] : "?", c.index, c.count);
          }
        }
      } else {
        std::fputs("; <unparseable header>\n", f);
      }
    } catch (...) {
      std::fputs("; <disassembler exception>\n", f);
    }

    std::fputs("; raw dwords:\n", f);
    const uint32_t* dw = static_cast<const uint32_t*>(code);
    for (size_t i = 0u; i < size / 4u; i++)
      std::fprintf(f, "%08x%s", dw[i], (i % 8u == 7u) ? "\n" : " ");
    std::fputc('\n', f);
    std::fclose(f);
  }


  void D3D9DeviceEx::DebugWriteShaderAlways(const std::string& name, const void* code, size_t size) {
    // Despite the name: only when D9MT_SHADER_DUMP_PATH or D9MT_DEBUG_FILE is set.
    DebugInitPath();
    if (m_dbg.shaderDir.empty())
      return;
    CreateDirectoryA(m_dbg.shaderDir.c_str(), nullptr);
    DebugWriteShader(name, code, size);
    Logger::warn(str::format("D9MT-DEBUG shader text: ", m_dbg.shaderDir, "\\", name, ".txt"));
  }


  template <typename Shader>
  std::string D3D9DeviceEx::DebugShaderName(Shader* shader) {
    if (!shader)
      return "FF";
    std::string name = shader->GetCommonShader()->GetName();
    if (m_dbg.shadersWritten.find(name) == m_dbg.shadersWritten.end()) {
      UINT size = 0u;
      if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size) {
        std::vector<uint8_t> code(size);
        if (SUCCEEDED(shader->GetFunction(code.data(), &size)))
          DebugWriteShader(name, code.data(), size);
      }
    }
    return name;
  }


  void D3D9DeviceEx::DebugOnDraw(D3DPRIMITIVETYPE type, UINT count, const char* api, UINT instances) {
    const uint32_t idx = m_dbg.drawIdx++;
    if (!m_dbg.dumpFile)
      return;

    auto& rs = m_state.renderStates;
    std::ostringstream ss;

    ss << "D" << idx << " " << api << " " << primName(type) << "x" << count;
    if (instances > 1u)
      ss << " i" << instances;

    ss << " vs=" << DebugShaderName(m_state.vertexShader.ptr())
       << " ps=" << DebugShaderName(m_state.pixelShader.ptr());

    D3D9CommonTexture* rtTex[caps::MaxSimultaneousRenderTargets] = { };
    for (uint32_t i = 0u; i < caps::MaxSimultaneousRenderTargets; i++) {
      if (!HasRenderTargetBound(i))
        continue;
      rtTex[i] = m_state.renderTargets[i]->GetCommonTexture();
      ss << " rt" << i << "=" << DebugSurfaceDesc(rtTex[i]);
    }

    D3D9CommonTexture* dsTex = nullptr;
    if (m_state.depthStencil != nullptr) {
      dsTex = m_state.depthStencil->GetCommonTexture();
      const D3D9_COMMON_TEXTURE_DESC* dd = dsTex->Desc();

      // Mirrors BindFramebuffer().
      bool bound = true;
      if (rtTex[0] != nullptr) {
        const D3D9_COMMON_TEXTURE_DESC* rd = rtTex[0]->Desc();
        bound = dd->Width >= rd->Width && dd->Height >= rd->Height
             && dd->MultiSample == rd->MultiSample;
        if (!bound) {
          uint32_t nrt = 0u;
          for (uint32_t i = 0u; i < caps::MaxSimultaneousRenderTargets; i++)
            nrt += m_state.renderTargets[i] != nullptr ? 1u : 0u;
          const bool rt0Written = (PSShaderMasks().rtMask & 1u) && rs[D3DRS_COLORWRITEENABLE];
          bound = nrt == 1u && rd->Width == 1u && rd->Height == 1u && !rt0Written;
        }
      }
      bool readOnly = m_textureSlotTracking.hazardDS != 0
        && (!rs[D3DRS_ZENABLE] || !rs[D3DRS_ZWRITEENABLE])
        && !dsTex->HasBeenTransitionedToHazardLayout();
      ss << " ds=" << DebugSurfaceDesc(dsTex) << ":" << (!bound ? "unbound" : readOnly ? "ro" : "rw");
    }

    const D3DVIEWPORT9& vp = m_state.viewport;
    ss << " vp=" << vp.X << "," << vp.Y << "," << vp.Width << "," << vp.Height
       << "," << vp.MinZ << "," << vp.MaxZ;

    ss << " z=" << rs[D3DRS_ZENABLE] << ":" << cmpName(rs[D3DRS_ZFUNC]) << ":w" << rs[D3DRS_ZWRITEENABLE];
    ss << " cull=" << cullName(rs[D3DRS_CULLMODE]);

    ss << " bl=" << (rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0);
    if (rs[D3DRS_ALPHABLENDENABLE]) {
      ss << ":" << blendName(rs[D3DRS_SRCBLEND]) << "/" << blendName(rs[D3DRS_DESTBLEND])
         << "/" << blendOpName(rs[D3DRS_BLENDOP]);
      if (rs[D3DRS_SEPARATEALPHABLENDENABLE])
        ss << " a=" << blendName(rs[D3DRS_SRCBLENDALPHA]) << "/" << blendName(rs[D3DRS_DESTBLENDALPHA])
           << "/" << blendOpName(rs[D3DRS_BLENDOPALPHA]);
    }
    ss << " cw=" << std::hex << rs[D3DRS_COLORWRITEENABLE] << std::dec;

    ss << " at=" << (rs[D3DRS_ALPHATESTENABLE] ? 1 : 0);
    if (rs[D3DRS_ALPHATESTENABLE])
      ss << ":" << cmpName(rs[D3DRS_ALPHAFUNC]) << ":" << rs[D3DRS_ALPHAREF];

    ss << " st=" << (rs[D3DRS_STENCILENABLE] ? 1 : 0);
    if (rs[D3DRS_STENCILENABLE]) {
      ss << ":" << cmpName(rs[D3DRS_STENCILFUNC]) << ":" << rs[D3DRS_STENCILREF]
         << ":" << std::hex << rs[D3DRS_STENCILMASK] << ":" << rs[D3DRS_STENCILWRITEMASK] << std::dec
         << ":" << stencilOpName(rs[D3DRS_STENCILPASS]) << "/" << stencilOpName(rs[D3DRS_STENCILFAIL])
         << "/" << stencilOpName(rs[D3DRS_STENCILZFAIL]);
      if (rs[D3DRS_TWOSIDEDSTENCILMODE])
        ss << ":2s(" << cmpName(rs[D3DRS_CCW_STENCILFUNC]) << ":" << stencilOpName(rs[D3DRS_CCW_STENCILPASS])
           << "/" << stencilOpName(rs[D3DRS_CCW_STENCILFAIL]) << "/" << stencilOpName(rs[D3DRS_CCW_STENCILZFAIL]) << ")";
    }

    ss << " sc=" << (rs[D3DRS_SCISSORTESTENABLE] ? 1 : 0);
    if (rs[D3DRS_SCISSORTESTENABLE]) {
      const RECT& r = m_state.scissorRect;
      ss << ":" << r.left << "," << r.top << "," << r.right << "," << r.bottom;
    }

    const float bias  = bit::cast<float>(rs[D3DRS_DEPTHBIAS]);
    const float slope = bit::cast<float>(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
    ss << " db=" << bias << "," << slope;
    ss << " clip=" << std::hex << rs[D3DRS_CLIPPLANEENABLE] << std::dec;
    ss << " srgb=" << (rs[D3DRS_SRGBWRITEENABLE] ? 1 : 0);

    if (!m_dbg.activeOcc.empty()) {
      ss << " occ=";
      for (size_t i = 0u; i < m_dbg.activeOcc.size(); i++)
        ss << (i ? "," : "") << "Q" << m_dbg.activeOcc[i].id;
    }

    const uint32_t usedSamplers = PSShaderMasks().samplerMask | VSShaderMasks().samplerMask;
    if (usedSamplers) {
      ss << " tex=";
      bool first = true;
      for (uint32_t s : bit::BitMask(usedSamplers)) {
        ss << (first ? "" : " ") << "s" << s << "=";
        first = false;
        D3D9CommonTexture* t = GetCommonTexture(m_state.textures[s]);
        if (!t) {
          ss << "null";
          continue;
        }
        ss << DebugSurfaceDesc(t);
        if (m_textureSlotTracking.depth & (1u << s))
          ss << ":shadow";
        if (t == dsTex)
          ss << ":=DS";
        for (uint32_t i = 0u; i < caps::MaxSimultaneousRenderTargets; i++) {
          if (rtTex[i] == t)
            ss << ":=RT" << i;
        }
      }
    }

    ss << "\n";
    std::fputs(ss.str().c_str(), m_dbg.dumpFile);
  }


  void D3D9DeviceEx::DebugQueryLine(const std::string& text) {
    if (m_dbg.dumpFile)
      std::fprintf(m_dbg.dumpFile, "  %s @D%u\n", text.c_str(), m_dbg.drawIdx);
    if (m_dbg.flags & d9mtdbg::OccTrace)
      DebugLog(str::format("f", m_dbg.frame, " @D", m_dbg.drawIdx, " ", text));
  }


  void D3D9DeviceEx::DebugOnQueryIssue(uint32_t id, bool begin, bool unread, uint32_t polls) {
    auto& active = m_dbg.activeOcc;
    auto it = std::find_if(active.begin(), active.end(),
      [id] (const D3D9DebugState::ActiveQuery& q) { return q.id == id; });

    if (begin) {
      if (it != active.end())
        active.erase(it);
      active.push_back({ id, m_dbg.drawIdx, m_dbg.frame });
      std::string text = str::format("Q", id, " BEGIN nested=", active.size() - 1u);
      if (unread)
        text += str::format(" (previous result never read, polls=", polls, ")");
      DebugQueryLine(text);
    } else {
      std::string draws = "?";
      if (it != active.end()) {
        if (it->frame == m_dbg.frame)
          draws = str::format(m_dbg.drawIdx - it->draw);
        active.erase(it);
      }
      DebugQueryLine(str::format("Q", id, " END draws=", draws));
    }
  }


  void D3D9DeviceEx::DebugOnQueryData(uint32_t id, HRESULT hr, DWORD samples, uint32_t polls,
                                      uint64_t endFrame, bool flush, const char* note) {
    DebugQueryLine(str::format("Q", id, " DATA ", hrName(hr), " samples=", samples,
      " polls=", polls, " lat=", m_dbg.frame - endFrame, "f",
      flush ? " flush" : "", note ? " " : "", note ? note : ""));
  }


  void D3D9DeviceEx::DebugSyncQuery() {
    Flush();
    SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
  }

}
