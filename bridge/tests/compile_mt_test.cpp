// Concurrency stress test for the bridge compile path, mirroring what the
// driver's compile workers do through winemetal.so (no Wine): many threads
// at once run the AIR thunk (sqlite cache), MTLDevice newLibrary and
// MTL4Compiler render PSO builds (pipeline archive serializer attached,
// flushed every few PSOs while other threads compile, or with
// D9MT_PSO_ASYNC_FLUSH=1 by the bridge's flush thread once builds pause).
//
//   compile_mt_test <winemetal.so> <workdir> <sm3 .bin>...
//
// Run it twice on the same workdir: the second run must hit the AIR cache
// and load the archives, produce byte-identical metallibs and build every
// PSO the first run built (recorded in <workdir>/results.txt).
// Exit status 0 = no failure / mismatch. Build with -fsanitize=thread
// against a TSan-built winemetal.so to check the unix side for races.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "../winemetal.h"
#include "../winemetal_thunks.h"
#include "../../airlib/d9mtair.h"

#include <ir/ir_builder.h>
#include <ir/ir_legalize.h>
#include <ir/ir_serialize.h>
#include <sm3/sm3_converter.h>
#include <util/util_byte_stream.h>

extern "C" __attribute__((visibility("default"))) int NtSetEvent(void*, void*) { return 0; }

typedef int (*unix_fn)(void*);
static unix_fn* g_funcs;
static std::atomic<int> g_failures = { 0 };

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); g_failures++; } while (0)

static std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return { };
  std::vector<uint8_t> data(size_t(f.tellg()));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
  return data;
}

static std::vector<uint8_t> sm3ToIr(const std::string& path) {
  std::vector<uint8_t> bytecode = readFile(path);
  dxbc_spv::util::ByteReader reader(bytecode.data(), bytecode.size());
  dxbc_spv::sm3::Converter::Options options = {};
  options.name = "main";
  dxbc_spv::ir::Builder builder;
  dxbc_spv::sm3::Converter converter(reader, options);
  if (!converter.convertShader(builder))
    return { };
  dxbc_spv::ir::legalizeIr(builder, dxbc_spv::ir::CompileOptions());
  dxbc_spv::ir::Serializer serializer(builder);
  std::vector<uint8_t> blob(serializer.computeSerializedSize());
  if (!serializer.serialize(blob.data(), blob.size()))
    return { };
  return blob;
}

// Minimal compile pool with the driver's semantics: urgent work jumps the
// queue, a waiter runs a still-queued job itself.
struct Job {
  std::function<void()> fn;
  std::atomic<int> state = { 0 };   // 0 pending, 1 running, 2 done
  std::mutex m;
  std::condition_variable cv;

  bool tryRun() {
    int e = 0;
    if (!state.compare_exchange_strong(e, 1)) return false;
    fn();
    { std::lock_guard<std::mutex> l(m); state = 2; }
    cv.notify_all();
    return true;
  }

  void wait() {
    if (state == 2 || tryRun()) return;
    std::unique_lock<std::mutex> l(m);
    cv.wait(l, [this] { return state == 2; });
  }
};

struct Pool {
  std::mutex m;
  std::condition_variable cv;
  std::deque<Job*> q;
  std::vector<std::thread> threads;
  bool stop = false;

  explicit Pool(unsigned n) {
    for (unsigned i = 0; i < n; i++)
      threads.emplace_back([this] {
        for (;;) {
          Job* j;
          { std::unique_lock<std::mutex> l(m);
            cv.wait(l, [this] { return stop || !q.empty(); });
            if (stop) return;
            j = q.front(); q.pop_front(); }
          j->tryRun();
        }
      });
  }

  void submit(Job* j, bool urgent) {
    { std::lock_guard<std::mutex> l(m);
      if (urgent) q.push_front(j); else q.push_back(j); }
    cv.notify_one();
  }

  ~Pool() {
    { std::lock_guard<std::mutex> l(m); stop = true; }
    cv.notify_all();
    for (auto& t : threads) t.join();
  }
};

struct Shader {
  std::string name;
  bool isPs = false;
  std::vector<uint8_t> ir;
  std::vector<uint8_t> metallib;   // first compile's output
  obj_handle_t library = 0;
  bool ok = false;
};

static bool airCompile(const std::vector<uint8_t>& ir, std::vector<uint8_t>& out) {
  std::vector<uint8_t> ml(512u * 1024u), refl(64u * 1024u);
  d9mt_air_compile_params p = {};
  p.ir_data = uint64_t(uintptr_t(ir.data()));
  p.ir_size = ir.size();
  p.entry_name = uint64_t(uintptr_t("main"));
  p.flags = D9MT_AIR_FLAG_FOLD_SPEC | D9MT_AIR_FLAG_ARG_BUFFER;
  p.pe_metallib_buffer = uint64_t(uintptr_t(ml.data()));
  p.pe_metallib_capacity = ml.size();
  p.pe_reflection_buffer = uint64_t(uintptr_t(refl.data()));
  p.pe_reflection_capacity = refl.size();
  g_funcs[155](&p);
  if (p.ret_status != D9MT_AIR_OK || !p.out_size)
    return false;
  ml.resize(p.out_size);
  out = std::move(ml);
  return true;
}

static obj_handle_t newLibrary(obj_handle_t device, const std::vector<uint8_t>& ml) {
  struct unixcall_generic_obj_uint64_obj_ret dd = {};
  dd.handle = obj_handle_t(uintptr_t(ml.data()));
  dd.arg = ml.size();
  g_funcs[118](&dd);
  if (!dd.ret) return 0;
  struct unixcall_mtldevice_newlibrary lib = {};
  lib.device = device;
  lib.data = dd.ret;
  g_funcs[25](&lib);
  obj_handle_t data = dd.ret;
  g_funcs[1](&data);
  if (lib.ret_error) { obj_handle_t e = lib.ret_error; g_funcs[1](&e); }
  return lib.ret_library;
}

static obj_handle_t buildPso(obj_handle_t compiler, obj_handle_t vs, obj_handle_t ps, uint32_t variant) {
  static const char* mainName = "main";
  static const WMTPixelFormat formats[] = {
    WMTPixelFormatBGRA8Unorm, WMTPixelFormatRGBA16Float, WMTPixelFormatRGBA8Unorm };
  WMTMtl4RenderPipelineInfo info = {};
  info.vertex_library = vs;
  info.fragment_library = ps;
  info.vertex_entry_name.set((void*)mainName);
  info.fragment_entry_name.set(ps ? (void*)mainName : nullptr);
  info.rasterization_enabled = 1;
  info.raster_sample_count = 1;
  info.input_primitive_topology = WMTPrimitiveTopologyClassUnspecified;
  for (uint32_t i = 0; i < 2; i++) {
    info.colors[i].pixel_format = formats[(variant + i) % 3];
    info.colors[i].write_mask = 0xf;
    info.colors[i].blending_enabled = (variant >> 1) & 1;
    info.colors[i].rgb_blend_operation = WMTBlendOperationAdd;
    info.colors[i].alpha_blend_operation = WMTBlendOperationAdd;
    info.colors[i].src_rgb_blend_factor = WMTBlendFactorSourceAlpha;
    info.colors[i].dst_rgb_blend_factor = WMTBlendFactorOneMinusSourceAlpha;
    info.colors[i].src_alpha_blend_factor = WMTBlendFactorOne;
    info.colors[i].dst_alpha_blend_factor = WMTBlendFactorZero;
  }
  info.vertex.num_attributes = 16;
  for (uint32_t i = 0; i < 16; i++)
    info.vertex.attributes[i] = { WMTAttributeFormatFloat4, uint16_t(16u * i), 3u };
  info.vertex.num_layouts = 4;
  info.vertex.layouts[3] = { 256u, WMTVertexStepFunctionPerVertex, 1u };

  struct unixcall_mtl4_newrenderpso call = {};
  call.compiler = compiler;
  call.info.set(&info);
  g_funcs[196](&call);
  if (call.ret_error) {
    if (getenv("MT_VERBOSE")) {
      char buf[1024] = {0};
      struct unixcall_generic_obj_obj_ret d = {}; d.handle = call.ret_error; g_funcs[28](&d);
      if (d.ret) {
        struct unixcall_nsstring_getcstring gc = {};
        gc.str = d.ret; gc.buffer_ptr = uint64_t(uintptr_t(buf));
        gc.max_length = sizeof(buf) - 1; gc.encoding = WMTUTF8StringEncoding; g_funcs[8](&gc);
      }
      fprintf(stderr, "pso error: %s\n", buf);
    }
    obj_handle_t e = call.ret_error; g_funcs[1](&e);
  }
  return call.ret_pso;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <winemetal.so> <workdir> <sm3 .bin>...\n", argv[0]);
    return 2;
  }
  const std::string work = argv[2];
  const unsigned nThreads = getenv("MT_THREADS") ? unsigned(atoi(getenv("MT_THREADS"))) : 6u;

  // Bridge reads these on first use.
  setenv("D9MT_AIR_CACHE", (work + "/air-cache.sqlite3").c_str(), 1);
  setenv("D9MT_PSO_ARCHIVE_DIR", (work + "/pso").c_str(), 1);
  setenv("D9MT_PSO_FLUSH_EVERY", "5", 1);

  void* bridge = dlopen(argv[1], RTLD_NOW);
  if (!bridge) { fprintf(stderr, "FAIL: dlopen: %s\n", dlerror()); return 1; }
  g_funcs = reinterpret_cast<unix_fn*>(dlsym(bridge, "__wine_unix_call_funcs"));
  if (!g_funcs) { fprintf(stderr, "FAIL: no call table\n"); return 1; }

  struct unixcall_generic_obj_ret devs = {};
  g_funcs[4](&devs);
  struct unixcall_generic_obj_uint64_obj_ret dev0 = {};
  dev0.handle = devs.ret;
  g_funcs[2](&dev0);
  obj_handle_t device = dev0.ret;
  if (!device) { fprintf(stderr, "FAIL: no Metal device\n"); return 1; }

  struct unixcall_generic_obj_obj_ret mk = {};
  mk.handle = device;
  g_funcs[161](&mk);
  obj_handle_t compiler = mk.ret;
  if (!compiler) { fprintf(stderr, "FAIL: newMTL4Compiler\n"); return 1; }

  std::vector<Shader> shaders;
  for (int i = 3; i < argc; i++) {
    Shader s;
    s.name = argv[i];
    s.name = s.name.substr(s.name.find_last_of('/') + 1);
    s.isPs = s.name.rfind("ps_", 0) == 0;
    shaders.push_back(std::move(s));
  }

  auto t0 = std::chrono::steady_clock::now();
  std::mutex resultMutex;
  std::map<std::string, std::string> results;

  {
    Pool pool(nThreads);

    // Phase A (SM3 -> IR) in parallel, then 3 concurrent AIR compiles of
    // every shader (same key hammered from several threads at once).
    std::vector<std::unique_ptr<Job>> phaseA, air;
    for (size_t i = 0; i < shaders.size(); i++) {
      auto* s = &shaders[i];
      std::string path = argv[3 + i];
      phaseA.emplace_back(new Job);
      phaseA.back()->fn = [s, path] { s->ir = sm3ToIr(path); };
      pool.submit(phaseA.back().get(), false);
    }

    std::mutex mlMutex;
    for (size_t i = 0; i < shaders.size(); i++) {
      auto* s = &shaders[i];
      Job* a = phaseA[i].get();
      for (int rep = 0; rep < 3; rep++) {
        air.emplace_back(new Job);
        air.back()->fn = [s, a, &mlMutex, device] {
          a->wait();
          if (s->ir.empty()) return;
          std::vector<uint8_t> ml;
          if (!airCompile(s->ir, ml)) return;
          obj_handle_t lib = newLibrary(device, ml);
          std::lock_guard<std::mutex> l(mlMutex);
          if (s->metallib.empty()) {
            s->metallib = ml;
            s->library = lib;
            s->ok = lib != 0;
          } else if (s->metallib != ml) {
            FAIL("%s: concurrent AIR compiles differ", s->name.c_str());
          }
        };
        pool.submit(air.back().get(), rep == 0);
      }
    }

    // "Render thread": waits on jobs in reverse order (steals queued ones).
    for (auto it = air.rbegin(); it != air.rend(); ++it)
      (*it)->wait();

    unsigned okShaders = 0;
    for (auto& s : shaders) {
      okShaders += s.ok;
      std::ostringstream h;
      h << "air " << s.name << " " << (s.ok ? "ok " : "fail ") << s.metallib.size();
      uint64_t fnv = 0xcbf29ce484222325ull;
      for (uint8_t b : s.metallib) fnv = (fnv ^ b) * 0x100000001b3ull;
      h << " " << std::hex << fnv;
      results[s.name] = h.str();
    }
    printf("shaders: %u/%zu compiled\n", okShaders, shaders.size());

    // PSOs: every VS x PS pair in 4 state variants, all in flight at once.
    std::vector<std::unique_ptr<Job>> psos;
    std::atomic<unsigned> psoOk = { 0 }, psoFail = { 0 };
    for (auto& vs : shaders) {
      if (vs.isPs || !vs.ok) continue;
      for (auto& ps : shaders) {
        if (!ps.isPs || !ps.ok) continue;
        for (uint32_t v = 0; v < 4; v++) {
          psos.emplace_back(new Job);
          auto key = vs.name + "+" + ps.name + "#" + std::to_string(v);
          psos.back()->fn = [&, key, v, vsLib = vs.library, psLib = ps.library] {
            obj_handle_t pso = buildPso(compiler, vsLib, psLib, v);
            (pso ? psoOk : psoFail)++;
            std::lock_guard<std::mutex> l(resultMutex);
            results["pso " + key] = pso ? "ok" : "fail";
          };
          pool.submit(psos.back().get(), true);
        }
      }
    }
    for (auto& j : psos)
      j->wait();
    printf("psos: %u ok, %u failed (of %zu)\n", psoOk.load(), psoFail.load(), psos.size());
  }

  const double ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - t0).count();
  printf("elapsed: %.1f ms (%u threads)\n", ms, nThreads);

  // With D9MT_PSO_ASYNC_FLUSH=1 the bridge flushes the archive on its own
  // thread once builds pause (2 s); this process must still run then. Wait
  // for this session's archive (without it, builds flushed it already).
  const auto tw = std::chrono::steady_clock::now();
  const std::string session = "-" + std::to_string(getpid()) + "-";
  bool flushed = false;
  while (!flushed && std::chrono::steady_clock::now() - tw < std::chrono::seconds(15)) {
    if (DIR* base = opendir((work + "/pso").c_str())) {
      while (dirent* d = readdir(base)) {
        if (strncmp(d->d_name, "os-", 3)) continue;
        if (DIR* sub = opendir((work + "/pso/" + d->d_name).c_str())) {
          while (dirent* f = readdir(sub)) {
            const std::string n = f->d_name;
            if (n.find(session) != std::string::npos && n.size() > 4 && n.compare(n.size() - 4, 4, ".sum") == 0)
              flushed = true;
          }
          closedir(sub);
        }
      }
      closedir(base);
    }
    if (!flushed)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (flushed)
    printf("archive flushed %.0f ms after the last build\n", std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - tw).count());
  else
    FAIL("no PSO archive of this session within 15 s of the last build");

  // Compare with the first run on this workdir: metallibs must be
  // byte-identical (AIR cache hits included) and no PSO that built before
  // may fail now. The reverse is allowed: an archive lookup skips Metal's
  // VS/PS interface validation, so a pair that failed to link cold can
  // "succeed" once both functions' binaries are in an archive.
  std::string resPath = work + "/results.txt";
  std::vector<uint8_t> prevBytes = readFile(resPath);
  if (!prevBytes.empty()) {
    std::map<std::string, std::string> prev;
    std::istringstream in(std::string(prevBytes.begin(), prevBytes.end()));
    for (std::string line; std::getline(in, line); ) {
      size_t eq = line.find(" = ");
      if (eq != std::string::npos)
        prev[line.substr(0, eq)] = line.substr(eq + 3);
    }
    unsigned archiveOnly = 0;
    for (auto& r : results) {
      auto it = prev.find(r.first);
      if (it == prev.end())
        FAIL("%s: missing in previous run", r.first.c_str());
      else if (r.first.rfind("pso ", 0) == 0 && it->second == "fail" && r.second == "ok")
        archiveOnly++;
      else if (it->second != r.second)
        FAIL("%s: '%s' now, '%s' before", r.first.c_str(), r.second.c_str(), it->second.c_str());
    }
    printf("compared with first run: %s (%u PSOs only via archive lookup)\n",
      g_failures ? "MISMATCH" : "consistent", archiveOnly);
  } else {
    std::ofstream out(resPath);
    for (auto& r : results) out << r.first << " = " << r.second << "\n";
    printf("results recorded to %s\n", resPath.c_str());
  }

  if (g_failures) {
    fprintf(stderr, "%d failure(s)\n", g_failures.load());
    return 1;
  }
  printf("OK\n");
  return 0;
}
