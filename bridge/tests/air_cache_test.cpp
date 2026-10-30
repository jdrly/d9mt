// AIR cache test harness (scripts/test-air-cache-lineage.sh, ADR-0006): the AIR codegen of a
// winemetal.so on SM3 shaders, without Wine. Each .bin goes through dxbc-spirv's sm3 front end and
// legalize (as the driver does), then, with six flag sets, through the bridge's AIR compile thunk
// (unix call 155, with the AIR cache D9MT_AIR_CACHE names), or with AIR_THUNK=0 through
// d9mtair_compile (no cache). Every output (metallib + reflection blob) is written to
// <outdir>/<bin>.f<flags>.{metallib,refl} and listed in <outdir>/manifest.txt with its status,
// sizes and SHA-256.
//   air_cache_test <winemetal.so> <outdir> <sm3 .bin>...
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../../airlib/d9mtair.h"

#include <ir/ir_builder.h>
#include <ir/ir_legalize.h>
#include <ir/ir_serialize.h>
#include <sm3/sm3_converter.h>
#include <util/util_byte_stream.h>

extern "C" __attribute__((visibility("default"))) int NtSetEvent(void*, void*) { return 0; }

static std::vector<uint8_t> readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string sha(const void* d, size_t n) {
  unsigned char h[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(d, (CC_LONG)n, h);
  char s[65];
  for (int i = 0; i < 32; i++) snprintf(s + 2 * i, 3, "%02x", h[i]);
  return s;
}

static std::vector<uint8_t> sm3ToIr(const std::vector<uint8_t>& code) {
  dxbc_spv::util::ByteReader reader(code.data(), code.size());
  dxbc_spv::sm3::Converter::Options options = {};
  options.name = "main";
  options.includeDebugNames = true;
  dxbc_spv::sm3::Converter converter(reader, options);
  dxbc_spv::ir::Builder builder;
  if (!converter.convertShader(builder)) return {};
  dxbc_spv::ir::CompileOptions compileOptions = {};
  dxbc_spv::ir::legalizeIr(builder, compileOptions);
  dxbc_spv::ir::Serializer serializer(builder);
  std::vector<uint8_t> blob(serializer.computeSerializedSize());
  if (!serializer.serialize(blob.data(), blob.size())) return {};
  return blob;
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: %s <so> <outdir> <bin>...\n", argv[0]); return 2; }
  void* so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!so) { fprintf(stderr, "FATAL: dlopen: %s\n", dlerror()); return 1; }
  auto compile = (int (*)(d9mt_air_compile_params*))dlsym(so, "d9mtair_compile");
  if (!getenv("AIR_THUNK") || strcmp(getenv("AIR_THUNK"), "0")) {
    auto funcs = (int (**)(void*))dlsym(so, "__wine_unix_call_funcs");
    if (!funcs) { fprintf(stderr, "FATAL: no call table\n"); return 1; }
    compile = (int (*)(d9mt_air_compile_params*))funcs[155];
  }
  auto release = (int (*)(d9mt_air_free_params*))dlsym(so, "d9mtair_free");
  if (!compile || !release) { fprintf(stderr, "FATAL: symbols\n"); return 1; }
  const std::string out = argv[2];
  FILE* man = fopen((out + "/manifest.txt").c_str(), "w");
  static const uint32_t variants[] = {
    0u, D9MT_AIR_FLAG_FOLD_SPEC, D9MT_AIR_FLAG_ARG_BUFFER, D9MT_AIR_FLAG_FAST_MATH,
    D9MT_AIR_FLAG_ARG_BUFFER | D9MT_AIR_FLAG_FAST_MATH,
    D9MT_AIR_FLAG_FOLD_SPEC | D9MT_AIR_FLAG_ARG_BUFFER | D9MT_AIR_FLAG_FAST_MATH,
  };
  int bad = 0;
  for (int i = 3; i < argc; i++) {
    std::string name = argv[i];
    name = name.substr(name.find_last_of('/') + 1);
    std::vector<uint8_t> ir = sm3ToIr(readFile(argv[i]));
    if (ir.empty()) { fprintf(man, "%s ir-failed\n", name.c_str()); bad++; continue; }
    for (uint32_t v : variants) {
      d9mt_air_compile_params p = {};
      p.ir_data = uint64_t(uintptr_t(ir.data()));
      p.ir_size = ir.size();
      p.entry_name = uint64_t(uintptr_t("main"));
      p.flags = v;
      compile(&p);
      std::string base = out + "/" + name + ".f" + std::to_string(v);
      if (p.ret_status != D9MT_AIR_OK) {
        fprintf(man, "%s.f%u status %u ir %s\n", name.c_str(), v, p.ret_status, sha(ir.data(), ir.size()).c_str());
        continue;
      }
      const void* ml = (const void*)uintptr_t(p.out_metallib);
      const void* rf = (const void*)uintptr_t(p.out_reflection);
      std::ofstream(base + ".metallib", std::ios::binary).write((const char*)ml, std::streamsize(p.out_size));
      std::ofstream(base + ".refl", std::ios::binary).write((const char*)rf, std::streamsize(p.out_reflection_size));
      fprintf(man, "%s.f%u ok ir %s metallib %llu %s refl %llu %s\n", name.c_str(), v, sha(ir.data(), ir.size()).c_str(),
              (unsigned long long)p.out_size, sha(ml, p.out_size).c_str(),
              (unsigned long long)p.out_reflection_size, sha(rf, p.out_reflection_size).c_str());
      d9mt_air_free_params f = {};
      f.ptr = p.out_metallib; release(&f);
      f.ptr = p.out_reflection; release(&f);
    }
  }
  fclose(man);
  return bad ? 1 : 0;
}
