/* Bridge bundle part 9b (docs/bridge-bundle.md): an MTL4 render pass through winemetal.so's own
 * thunks, so the pipeline and the encoder get the bridge's validation labels. Its fragment
 * shader reads src[idx] from a 1000-byte buffer: idx 10 is in bounds, idx 266 reads 64 bytes past
 * its end, inside the buffer's own page (mapped memory). The out-of-bounds read is only ever run
 * under shader validation, which zero-fills it and reports it on stderr
 * (MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1); scripts/test-validation-labels.sh checks that the
 * report names "d9mt vs:…/vmain fs:…/fmain" and "d9mt enc#…".
 *   validation_label_test <winemetal.so> <val.metallib> <in|oob> */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <unistd.h>
#include <vector>

#include "../winemetal.h"
#include "../winemetal_thunks.h"

extern "C" __attribute__((visibility("default"))) int NtSetEvent(void*, void*) { return 0; }

typedef int (*unix_fn)(void*);
static unix_fn* funcs;

static obj_handle_t make(int call, obj_handle_t on, const char* what) {
  struct unixcall_generic_obj_obj_ret mk = {};
  mk.handle = on;
  funcs[call](&mk);
  if (!mk.ret) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
  return mk.ret;
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: validation_label_test <winemetal.so> <val.metallib> <in|oob>\n"); return 2; }
  const bool oob = !strcmp(argv[3], "oob");
  if (oob && !getenv("MTL_SHADER_VALIDATION")) {   /* the out-of-bounds read only ever under validation */
    fprintf(stderr, "FAIL: oob without MTL_SHADER_VALIDATION refused\n");
    return 2;
  }
  void* bridge = dlopen(argv[1], RTLD_NOW);
  if (!bridge) { fprintf(stderr, "FAIL: dlopen bridge: %s\n", dlerror()); return 1; }
  funcs = reinterpret_cast<unix_fn*>(dlsym(bridge, "__wine_unix_call_funcs"));
  if (!funcs) { fprintf(stderr, "FAIL: no call table\n"); return 1; }

  struct unixcall_generic_obj_ret devs = {};
  funcs[4](&devs);
  struct unixcall_generic_obj_uint64_obj_ret dev0 = {};
  dev0.handle = devs.ret; dev0.arg = 0;
  funcs[2](&dev0);
  if (!dev0.ret) { fprintf(stderr, "FAIL: no Metal device\n"); return 1; }
  const obj_handle_t device = dev0.ret;

  /* the library, through the bridge (which takes its identity there) */
  std::ifstream f(argv[2], std::ios::binary | std::ios::ate);
  if (!f) { fprintf(stderr, "FAIL: cannot read %s\n", argv[2]); return 1; }
  std::vector<uint8_t> bytes(size_t(f.tellg()));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
  struct unixcall_generic_obj_uint64_obj_ret dd = {};
  dd.handle = obj_handle_t(uintptr_t(bytes.data())); dd.arg = bytes.size();
  funcs[118](&dd);
  struct unixcall_mtldevice_newlibrary libc = {};
  libc.device = device; libc.data = dd.ret;
  funcs[25](&libc);
  if (!libc.ret_library) { fprintf(stderr, "FAIL: newLibrary\n"); return 1; }

  const obj_handle_t queue4 = make(157, device, "newMTL4CommandQueue");
  const obj_handle_t allocator = make(158, device, "newCommandAllocator");
  const obj_handle_t cmdbuf = make(159, device, "newMTL4CommandBuffer");
  const obj_handle_t compiler = make(161, device, "newMTL4Compiler");

  WMTMtl4RenderPipelineInfo p4 = {};
  p4.colors[0].pixel_format = WMTPixelFormatBGRA8Unorm;
  p4.colors[0].write_mask = 0xf;
  p4.vertex_library = libc.ret_library;
  p4.fragment_library = libc.ret_library;
  const char* vname = "vmain";
  const char* fname = "fmain";
  p4.vertex_entry_name.set((void*)vname);
  p4.fragment_entry_name.set((void*)fname);
  p4.raster_sample_count = 1;
  p4.input_primitive_topology = WMTPrimitiveTopologyClassUnspecified;
  p4.rasterization_enabled = 1;
  struct unixcall_mtl4_newrenderpso pso = {};
  pso.compiler = compiler; pso.info.set(&p4);
  funcs[196](&pso);
  if (!pso.ret_pso) { fprintf(stderr, "FAIL: render PSO\n"); return 1; }

  /* src: 1000 bytes (250 floats 0..249), idx: 4 bytes, both shared */
  WMTBufferInfo sb = {}; sb.length = 1000; sb.options = (enum WMTResourceOptions)0;
  struct unixcall_mtldevice_newbuffer sbc = {}; sbc.device = device; sbc.info.set(&sb); funcs[18](&sbc);
  WMTBufferInfo ib = {}; ib.length = 4; ib.options = (enum WMTResourceOptions)0;
  struct unixcall_mtldevice_newbuffer ibc = {}; ibc.device = device; ibc.info.set(&ib); funcs[18](&ibc);
  if (!sbc.ret || !ibc.ret || !sb.memory.ptr || !ib.memory.ptr) { fprintf(stderr, "FAIL: buffers\n"); return 1; }
  for (int i = 0; i < 250; i++) static_cast<float*>(sb.memory.ptr)[i] = float(i);
  *static_cast<uint32_t*>(ib.memory.ptr) = oob ? 266u : 10u;

  WMTTextureInfo t4 = {};
  t4.pixel_format = WMTPixelFormatBGRA8Unorm;
  t4.width = 4; t4.height = 4; t4.depth = 1; t4.array_length = 1;
  t4.type = WMTTextureType2D; t4.mipmap_level_count = 1; t4.sample_count = 1;
  t4.usage = (enum WMTTextureUsage)(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  t4.options = WMTResourceStorageModePrivate;
  struct unixcall_mtldevice_newtexture tc = {}; tc.device = device; tc.info.set(&t4); funcs[21](&tc);
  if (!tc.ret) { fprintf(stderr, "FAIL: texture\n"); return 1; }

  WMTResidencySetInfo rsInfo = {}; rsInfo.initial_capacity = 8;
  struct unixcall_mtl4_new_from_desc rsc = {}; rsc.device = device; rsc.info.set(&rsInfo); funcs[162](&rsc);
  if (!rsc.ret) { fprintf(stderr, "FAIL: residency set\n"); return 1; }
  struct unixcall_generic_obj_obj_noret add = {};
  add.handle = rsc.ret;
  add.arg = sbc.ret; funcs[197](&add);
  add.arg = ibc.ret; funcs[197](&add);
  add.arg = tc.ret; funcs[197](&add);
  struct unixcall_generic_obj_noret rsop = {}; rsop.handle = rsc.ret; funcs[198](&rsop); funcs[199](&rsop);

  WMTArgTableInfo atInfo = {};
  atInfo.max_buffers = 4; atInfo.max_textures = 1; atInfo.max_samplers = 1;
  atInfo.initialize_bindings = 1; atInfo.support_attribute_strides = 1;
  struct unixcall_mtl4_new_from_desc atc = {}; atc.device = device; atc.info.set(&atInfo); funcs[160](&atc);
  if (!atc.ret) { fprintf(stderr, "FAIL: argument table\n"); return 1; }
  struct unixcall_mtl4_argtable_setaddress sa = {};
  sa.table = atc.ret; sa.stride = 0;
  sa.gpu_address = sb.gpu_address; sa.index = 0; funcs[172](&sa);
  sa.gpu_address = ib.gpu_address; sa.index = 1; funcs[172](&sa);

  struct unixcall_generic_obj_obj_noret begin = {}; begin.handle = cmdbuf; begin.arg = allocator; funcs[164](&begin);
  struct unixcall_generic_obj_obj_noret use = {}; use.handle = cmdbuf; use.arg = rsc.ret; funcs[166](&use);

  WMTRenderPassInfo pass = {};
  pass.colors[0].texture = tc.ret;
  pass.colors[0].load_action = WMTLoadActionClear;
  pass.colors[0].store_action = WMTStoreActionStore;
  pass.colors[0].clear_color = { 0.1, 0.2, 0.3, 1.0 };
  pass.default_raster_sample_count = 1;
  pass.render_target_width = 4; pass.render_target_height = 4;
  struct unixcall_generic_obj_uint64_obj_ret enc = {}; enc.handle = cmdbuf; enc.arg = uint64_t(uintptr_t(&pass));
  funcs[176](&enc);
  if (!enc.ret) { fprintf(stderr, "FAIL: render encoder\n"); return 1; }
  struct unixcall_mtl4_setargumenttable sat = {}; sat.encoder = enc.ret; sat.table = atc.ret; sat.stages = 1 | 2;
  funcs[177](&sat);
  struct unixcall_generic_obj_obj_noret setp = {}; setp.handle = enc.ret; setp.arg = pso.ret_pso; funcs[178](&setp);
  struct unixcall_mtl4_setviewport vp = {}; vp.encoder = enc.ret; vp.viewport = { 0, 0, 4, 4, 0, 1 }; funcs[179](&vp);
  struct unixcall_mtl4_setscissorrect sc = {}; sc.encoder = enc.ret; sc.rect = { 0, 0, 4, 4 }; funcs[180](&sc);
  struct unixcall_mtl4_draw draw = {};
  draw.encoder = enc.ret; draw.primitive = WMTPrimitiveTypeTriangle;
  draw.vertex_start = 0; draw.vertex_count = 3; draw.instance_count = 1; draw.base_instance = 0;
  funcs[187](&draw);
  struct unixcall_generic_obj_noret endEnc = {}; endEnc.handle = enc.ret; funcs[33](&endEnc);
  struct unixcall_generic_obj_noret endCb = {}; endCb.handle = cmdbuf; funcs[165](&endCb);
  obj_handle_t cbs[1] = { cmdbuf };
  struct unixcall_mtl4_queue_commit commit = {}; commit.queue = queue4; commit.cmdbufs.set(cbs); commit.count = 1;
  funcs[167](&commit);

  const obj_handle_t event = make(15, device, "newSharedEvent");
  struct unixcall_generic_obj_obj_uint64_noret sig = {}; sig.handle = queue4; sig.arg0 = event; sig.arg1 = 1; funcs[170](&sig);
  struct unixcall_mtlsharedevent_waituntilsignaledvalue w = {}; w.event = event; w.value = 1; w.timeout_ms = 5000;
  funcs[130](&w);
  if (!w.ret_timeout) { fprintf(stderr, "FAIL: the pass did not complete\n"); return 1; }
  usleep(300000);
  printf("VALTEST %s done\n", oob ? "oob" : "in");
  return 0;
}
