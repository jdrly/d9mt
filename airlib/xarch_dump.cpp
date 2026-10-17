// Cross-architecture determinism dump for the shader compile path
// (scripts/run-air-xarch.sh). Built once per unix-side architecture, it runs
// the d3d9.dll front end (SM3 -> dxbc-spirv IR: converter, legalize,
// serialize; for pixel shaders the deserialize/LowerIoPass/re-serialize of
// getFinalIr) and the unix-side compile core (d9mtair_compile, dlopened from
// winemetal.so or d9mtair.so), and prints a manifest line with SHA-256s of the
// IR, metallib and reflection bytes for every shader and variant. The
// manifests of two architectures must be identical.
//
//   xarch_dump <winemetal.so|d9mtair.so> <out-dir|-> <shader.bin>...
//
// out-dir keeps every artifact (for diffing a mismatch); "-" keeps none.
// Each shader runs in a forked child: dxbc-spirv asserts on some malformed
// input, and an abort is recorded ("abort <signal>") instead of ending the run,
// so failures have to match across architectures as well.
// Variants: converter options (fastFloatEmulation, isSWVP for vertex
// shaders, forceDynamicTextureType for pixel shaders, flat interpolation of
// all inputs for pixel shaders) x compile flags (FOLD_SPEC always, as in
// d3d9_shader_compiler.cpp, with and without ARG_BUFFER and FAST_MATH).

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ir/ir_builder.h>
#include <ir/ir_legalize.h>
#include <ir/ir_serialize.h>
#include <ir/passes/ir_pass_lower_io.h>
#include <sm3/sm3_converter.h>
#include <util/util_byte_stream.h>

#include "d9mtair.h"

namespace {

using CompileFn = int (*)(d9mt_air_compile_params*);
using FreeFn    = int (*)(d9mt_air_free_params*);

std::string sha256(const void* data, size_t size) {
  unsigned char md[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(data, CC_LONG(size), md);
  static const char hex[] = "0123456789abcdef";
  std::string s;
  for (unsigned char c : md) {
    s += hex[c >> 4];
    s += hex[c & 15];
  }
  return s;
}

void writeFile(const std::string& path, const void* data, size_t size) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data), std::streamsize(size));
}

struct FrontVariant {
  const char* name;
  bool fastFloat;
  bool swvp;          // vertex shaders only
  bool dynamicTex;    // pixel shaders only
  bool flat;          // pixel shaders only: LowerIoPass::enableFlatInterpolation(~0u)
};

const FrontVariant kFront[] = {
  { "base",      false, false, false, false },
  { "fastfloat", true,  false, false, false },
  { "swvp",      false, true,  false, false },
  { "dyntex",    false, false, true,  false },
  { "flat",      false, false, false, true  },
};

const uint32_t kFlags[] = {
  D9MT_AIR_FLAG_FOLD_SPEC | D9MT_AIR_FLAG_ARG_BUFFER,
  D9MT_AIR_FLAG_FOLD_SPEC,
  D9MT_AIR_FLAG_FOLD_SPEC | D9MT_AIR_FLAG_ARG_BUFFER | D9MT_AIR_FLAG_FAST_MATH,
  D9MT_AIR_FLAG_FOLD_SPEC | D9MT_AIR_FLAG_FAST_MATH,
};

// Front end as d3d9_shader_compiler.cpp runs it (minus the driver's
// D3D9ShaderLowerLegacyInputPass, which needs the driver's shader analysis).
bool frontEnd(const std::vector<uint8_t>& bytecode, const FrontVariant& v, bool isVs,
              std::vector<uint8_t>& ir) {
  using namespace dxbc_spv;

  sm3::Converter::Options opts = {};
  opts.name = "main";
  opts.includeDebugNames = true;
  opts.fastFloatEmulation = v.fastFloat;
  opts.isSWVP = v.swvp && isVs;
  opts.forceDynamicTextureType = v.dynamicTex && !isVs;

  util::ByteReader reader(bytecode.data(), bytecode.size());
  sm3::Converter converter(reader, opts);
  ir::Builder builder;
  if (!converter.convertShader(builder))
    return false;

  ir::CompileOptions compileOptions = {};
  ir::legalizeIr(builder, compileOptions);

  ir::Serializer ser(builder);
  std::vector<uint8_t> phaseA(ser.computeSerializedSize());
  if (!ser.serialize(phaseA.data(), phaseA.size()))
    return false;

  if (isVs) {
    ir = std::move(phaseA);
    return true;
  }

  // getFinalIr for a pixel shader without a paired vertex shader
  ir::Builder final;
  ir::Deserializer deser(phaseA.data(), phaseA.size());
  if (!deser.deserialize(final))
    return false;
  ir::LowerIoPass ioPass(final);
  if (v.flat)
    ioPass.enableFlatInterpolation(~0u);
  ir::Serializer ser2(final);
  ir.resize(ser2.computeSerializedSize());
  return ser2.serialize(ir.data(), ir.size());
}

}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <winemetal.so|d9mtair.so> <out-dir|-> <shader.bin>...\n", argv[0]);
    return 2;
  }
  void* lib = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
  if (!lib) {
    std::fprintf(stderr, "FATAL: dlopen: %s\n", dlerror());
    return 1;
  }
  auto compileFn = reinterpret_cast<CompileFn>(dlsym(lib, "d9mtair_compile"));
  auto freeFn = reinterpret_cast<FreeFn>(dlsym(lib, "d9mtair_free"));
  if (!compileFn || !freeFn) {
    std::fprintf(stderr, "FATAL: d9mtair_compile/d9mtair_free not exported\n");
    return 1;
  }
  const std::string outDir = argv[2];
  const bool keep = outDir != "-";

  unsigned shaders = 0, aborts = 0;
  for (int a = 3; a < argc; a++) {
    std::ifstream f(argv[a], std::ios::binary | std::ios::ate);
    if (!f) {
      std::fprintf(stderr, "FATAL: cannot read %s\n", argv[a]);
      return 1;
    }
    std::vector<uint8_t> bytecode(size_t(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytecode.data()), std::streamsize(bytecode.size()));
    std::string name = argv[a];
    name = name.substr(name.find_last_of('/') + 1);
    const bool isVs = bytecode.size() >= 4 && bytecode[2] == 0xfe && bytecode[3] == 0xff;
    shaders++;

    std::fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
      std::perror("fork");
      return 1;
    }
    if (pid > 0) {
      int st = 0;
      waitpid(pid, &st, 0);
      if (WIFSIGNALED(st)) {
        std::printf("%s abort %d\n", name.c_str(), WTERMSIG(st));
        aborts++;
      } else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        std::printf("%s exit %d\n", name.c_str(), WEXITSTATUS(st));
        aborts++;
      }
      continue;
    }

    for (const FrontVariant& v : kFront) {
      if ((v.swvp && !isVs) || ((v.dynamicTex || v.flat) && isVs))
        continue;
      std::vector<uint8_t> ir;
      if (!frontEnd(bytecode, v, isVs, ir)) {
        std::printf("%s %s reject\n", name.c_str(), v.name);
        continue;
      }
      std::printf("%s %s ir %zu %s\n", name.c_str(), v.name, ir.size(), sha256(ir.data(), ir.size()).c_str());
      if (keep)
        writeFile(outDir + "/" + name + "." + v.name + ".ir", ir.data(), ir.size());

      for (uint32_t flags : kFlags) {
        static const char entry[] = "main";
        d9mt_air_compile_params p = {};
        p.ir_data = uint64_t(uintptr_t(ir.data()));
        p.ir_size = ir.size();
        p.entry_name = uint64_t(uintptr_t(entry));
        p.flags = flags;
        compileFn(&p);
        if (p.ret_status != D9MT_AIR_OK) {
          std::printf("%s %s f%u status %u\n", name.c_str(), v.name, flags, p.ret_status);
            continue;
        }
        const void* ml = reinterpret_cast<const void*>(uintptr_t(p.out_metallib));
        const void* rf = reinterpret_cast<const void*>(uintptr_t(p.out_reflection));
        std::printf("%s %s f%u metallib %llu %s refl %llu %s\n", name.c_str(), v.name, flags,
          (unsigned long long)p.out_size, sha256(ml, p.out_size).c_str(),
          (unsigned long long)p.out_reflection_size, sha256(rf, p.out_reflection_size).c_str());
        if (keep) {
          std::string base = outDir + "/" + name + "." + v.name + ".f" + std::to_string(flags);
          writeFile(base + ".metallib", ml, p.out_size);
          writeFile(base + ".refl", rf, p.out_reflection_size);
        }
        d9mt_air_free_params fp = {};
        fp.ptr = p.out_metallib;
        freeFn(&fp);
        fp.ptr = p.out_reflection;
        freeFn(&fp);
      }
    }
    std::fflush(stdout);
    _exit(0);
  }
  std::fprintf(stderr, "xarch_dump: %u shaders, %u aborted\n", shaders, aborts);
  return 0;
}
