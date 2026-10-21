// archfirst_test <check|time> <workdir>: bridge bundle part 3 verification (docs/bridge-bundle.md,
// "Verification test first"). Native Metal 4, no Wine, no window.
//   check (correctness, any time):
//     1. PSOs built through a serializer-attached MTL4Compiler, flushed to an archive;
//     2. that archive loaded: every descriptor comes back from
//        [MTL4Archive newRenderPipelineStateWithDescriptor:] (a hit), a descriptor that was
//        never built returns nil (a miss) and says why in the error;
//     3. a PSO from the archive renders the same pixels as the compiler's PSO;
//     4. hits from the archive are not captured: a second serializer, attached to a second
//        compiler, sees nothing from the archive hits (its flush fails or is empty), while one
//        compiler build after them is captured;
//     5. 4 threads look up concurrently (run the binary under TSan for races);
//     6. lookups after the archive's file (and its .sum) was unlinked: a consolidation
//        deletes the parts this process (and others) still look up in. Every lookup must
//        still hit and render the same pixels (MTL4Archive must not reopen the URL lazily).
//   clang -arch x86_64 -O1 bridge/tests/archfirst_test.m -framework Metal -framework Foundation -o archfirst_test
//   (add -g -fsanitize=thread for the race check)
//   mem <direct|compiler|probe>, peak <chunk>, archload: memory of live PSOs by path, a
//   consolidation's peak footprint by chunk size, the startup cost per archive (see below).
//   time (needs a quiet window): hit cost via MTL4Archive vs via a compiler with
//     lookupArchives, and the miss cost with 1, 8 and 64 archives loaded.
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <mach/mach.h>

static double now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static const char *kSrc =
  "#include <metal_stdlib>\nusing namespace metal;\n"
  "vertex float4 vs(uint id [[vertex_id]]) { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2 - 1, 0.5, 1); }\n"
  "fragment float4 fs() { return float4(0.75, 0.25, 0.5, 1.0); }\n";

static id<MTLDevice> g_dev;
static id<MTLLibrary> g_lib;

static MTL4RenderPipelineDescriptor *desc(int n) {
  MTL4RenderPipelineDescriptor *d = [[MTL4RenderPipelineDescriptor alloc] init];
  MTL4LibraryFunctionDescriptor *v = [[MTL4LibraryFunctionDescriptor alloc] init];
  v.library = g_lib; v.name = @"vs";
  MTL4LibraryFunctionDescriptor *f = [[MTL4LibraryFunctionDescriptor alloc] init];
  f.library = g_lib; f.name = @"fs";
  d.vertexFunctionDescriptor = v; d.fragmentFunctionDescriptor = f;
  [v release]; [f release];
  d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  d.colorAttachments[0].blendingState = MTL4BlendStateEnabled;
  d.colorAttachments[0].sourceRGBBlendFactor = (MTLBlendFactor)(1 + n % 11);
  d.colorAttachments[0].destinationRGBBlendFactor = (MTLBlendFactor)(1 + n / 11 % 11);
  d.colorAttachments[0].rgbBlendOperation = (MTLBlendOperation)(n / 121 % 5);
  d.colorAttachments[0].writeMask = (MTLColorWriteMask)(1 + n / 605 % 15);
  return d;
}

static id<MTL4Compiler> compiler(id<MTL4PipelineDataSetSerializer> ser) {
  MTL4CompilerDescriptor *cd = [[MTL4CompilerDescriptor alloc] init];
  if (ser) cd.pipelineDataSetSerializer = ser;
  NSError *err = nil;
  id<MTL4Compiler> c = [g_dev newCompilerWithDescriptor:cd error:&err];
  [cd release];
  return c;
}

static id<MTL4PipelineDataSetSerializer> serializer(void) {
  MTL4PipelineDataSetSerializerDescriptor *sd = [[MTL4PipelineDataSetSerializerDescriptor alloc] init];
  sd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
  id<MTL4PipelineDataSetSerializer> s = [g_dev newPipelineDataSetSerializerWithDescriptor:sd];
  [sd release];
  return s;
}

/* An archive of PSOs first..first+count-1 at path. */
static id<MTL4Archive> make_archive(NSString *path, int first, int count) {
  id<MTL4PipelineDataSetSerializer> ser = serializer();
  id<MTL4Compiler> c = compiler(ser);
  for (int n = first; n < first + count; n++) {
    MTL4RenderPipelineDescriptor *d = desc(n);
    [[c newRenderPipelineStateWithDescriptor:d compilerTaskOptions:nil error:nil] release];
    [d release];
  }
  NSError *err = nil;
  if (![ser serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:path] error:&err])
    printf("FAIL serialize %s: %s\n", path.UTF8String, err.localizedDescription.UTF8String);
  [c release]; [ser release];
  return [g_dev newArchiveWithURL:[NSURL fileURLWithPath:path] error:&err];
}

static uint32_t render(id<MTLRenderPipelineState> pso) {
  MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:4 height:4 mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
  id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
  id<MTLCommandQueue> q = [g_dev newCommandQueue];
  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
  rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1.0);
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLCommandBuffer> cb = [q commandBuffer];
  id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
  [e setRenderPipelineState:pso];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
  uint32_t px = 0;
  [t getBytes:&px bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0];
  [t release]; [q release];
  return px;
}

typedef struct { id<MTL4Archive> ar; int first, count, hits; } LookupJob;
static void *lookup_thread(void *p) {
  LookupJob *j = p;
  @autoreleasepool {
    for (int r = 0; r < 4; r++)
      for (int n = j->first; n < j->first + j->count; n++) {
        MTL4RenderPipelineDescriptor *d = desc(n);
        id<MTLRenderPipelineState> s = [j->ar newRenderPipelineStateWithDescriptor:d error:nil];
        if (s) j->hits++;
        [s release]; [d release];
      }
  }
  return NULL;
}

static void check(NSString *dir) {
  const int N = 64;
  id<MTL4Archive> ar = make_archive([dir stringByAppendingPathComponent:@"a.mtl4archive"], 0, N);
  CHECK(ar != nil, "archive loads");
  if (!ar) return;
  int hits = 0;
  for (int n = 0; n < N; n++) {
    MTL4RenderPipelineDescriptor *d = desc(n);
    NSError *err = nil;
    id<MTLRenderPipelineState> s = [ar newRenderPipelineStateWithDescriptor:d error:&err];
    if (s) hits++;
    [s release]; [d release];
  }
  CHECK(hits == N, "2. hits: %d of %d archived descriptors come back", hits, N);
  NSError *err = nil;
  MTL4RenderPipelineDescriptor *md = desc(N + 7);
  double t0 = now_ms();
  id<MTLRenderPipelineState> miss = [ar newRenderPipelineStateWithDescriptor:md error:&err];
  printf("2. miss: %s (%.2f ms, error: %s)\n", miss ? "a PSO (!)" : "nil", now_ms() - t0,
         err ? err.localizedDescription.UTF8String : "none");
  CHECK(miss == nil, "2. a descriptor never built is a miss");
  [miss release]; [md release];

  /* 3. the same pixels as the compiler's PSO */
  id<MTL4Compiler> plain = compiler(nil);
  MTL4RenderPipelineDescriptor *d5 = desc(5);
  id<MTLRenderPipelineState> fromArchive = [ar newRenderPipelineStateWithDescriptor:d5 error:nil];
  id<MTLRenderPipelineState> fromCompiler = [plain newRenderPipelineStateWithDescriptor:d5 compilerTaskOptions:nil error:nil];
  const uint32_t pa = render(fromArchive), pc = render(fromCompiler);
  printf("3. pixel from the archive's PSO %08x, from the compiler's %08x\n", pa, pc);
  CHECK(fromArchive && fromCompiler && pa == pc, "3. archive PSO renders like the compiler's");
  [fromArchive release]; [fromCompiler release]; [d5 release];

  /* 4. archive hits are not captured; a compiler build is */
  id<MTL4PipelineDataSetSerializer> ser = serializer();
  id<MTL4Compiler> cap = compiler(ser);
  for (int n = 0; n < N; n++) {
    MTL4RenderPipelineDescriptor *d = desc(n);
    [[ar newRenderPipelineStateWithDescriptor:d error:nil] release];
    [d release];
  }
  NSString *p0 = [dir stringByAppendingPathComponent:@"after-hits.mtl4archive"];
  err = nil;
  BOOL ok0 = [ser serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:p0] error:&err];
  NSDictionary *a0 = [[NSFileManager defaultManager] attributesOfItemAtPath:p0 error:nil];
  printf("4. after %d archive hits: flush %s, %llu bytes%s%s\n", N, ok0 ? "ok" : "failed",
         (unsigned long long)[a0 fileSize], err ? ", " : "", err ? err.localizedDescription.UTF8String : "");
  MTL4RenderPipelineDescriptor *dn = desc(N + 11);
  [[cap newRenderPipelineStateWithDescriptor:dn compilerTaskOptions:nil error:nil] release];
  [dn release];
  NSString *p1 = [dir stringByAppendingPathComponent:@"after-build.mtl4archive"];
  BOOL ok1 = [ser serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:p1] error:nil];
  NSDictionary *a1 = [[NSFileManager defaultManager] attributesOfItemAtPath:p1 error:nil];
  printf("4. after 1 compiler build: flush %s, %llu bytes\n", ok1 ? "ok" : "failed", (unsigned long long)[a1 fileSize]);
  id<MTL4Archive> one = ok1 ? [g_dev newArchiveWithURL:[NSURL fileURLWithPath:p1] error:nil] : nil;
  int cappedHits = 0;
  for (int n = 0; one && n < N; n++) {
    MTL4RenderPipelineDescriptor *d = desc(n);
    id<MTLRenderPipelineState> s = [one newRenderPipelineStateWithDescriptor:d error:nil];
    if (s) cappedHits++;
    [s release]; [d release];
  }
  MTL4RenderPipelineDescriptor *dn2 = desc(N + 11);
  id<MTLRenderPipelineState> built = one ? [one newRenderPipelineStateWithDescriptor:dn2 error:nil] : nil;
  printf("4. that archive holds %d of the %d archive-hit PSOs and %s the built one\n", cappedHits, N, built ? "holds" : "lacks");
  CHECK(cappedHits == 0 && built, "4. only the compiler build was captured");
  [built release]; [dn2 release]; [one release]; [cap release]; [ser release]; [plain release];

  /* 5. concurrent lookups */
  pthread_t th[4]; LookupJob jobs[4];
  for (int i = 0; i < 4; i++) { jobs[i] = (LookupJob){ar, i * 16, 16, 0}; pthread_create(&th[i], NULL, lookup_thread, &jobs[i]); }
  int all = 0;
  for (int i = 0; i < 4; i++) { pthread_join(th[i], NULL); all += jobs[i].hits; }
  CHECK(all == 4 * 4 * 16, "5. concurrent lookups: %d of %d hits", all, 4 * 4 * 16);
  [ar release];

  /* 6. unlinked while loaded: a fresh archive, looked up once, then its file deleted */
  NSString *up = [dir stringByAppendingPathComponent:@"unlinked.mtl4archive"];
  id<MTL4Archive> ua = make_archive(up, 200, N);
  CHECK(ua != nil, "6. archive loads");
  MTL4RenderPipelineDescriptor *d0 = desc(200);
  [[ua newRenderPipelineStateWithDescriptor:d0 error:nil] release];
  [d0 release];
  CHECK(unlink(up.fileSystemRepresentation) == 0, "6. unlink");
  unlink([up stringByAppendingString:@".sum"].fileSystemRepresentation);
  int uhits = 0;
  for (int n = 200; n < 200 + N; n++) {
    MTL4RenderPipelineDescriptor *d = desc(n);
    id<MTLRenderPipelineState> st = [ua newRenderPipelineStateWithDescriptor:d error:nil];
    if (st) uhits++;
    if (n == 205 && st) {
      id<MTL4Compiler> pc = compiler(nil);
      id<MTLRenderPipelineState> ref = [pc newRenderPipelineStateWithDescriptor:d compilerTaskOptions:nil error:nil];
      const uint32_t pu = render(st), pr = render(ref);
      printf("6. pixel from the unlinked archive's PSO %08x, from the compiler's %08x\n", pu, pr);
      CHECK(ref && pu == pr, "6. unlinked archive PSO renders like the compiler's");
      [ref release]; [pc release];
    }
    [st release]; [d release];
  }
  printf("6. after unlink: %d of %d lookups hit\n", uhits, N);
  CHECK(uhits == N, "6. every lookup still hits after the unlink");
  [ua release];
}

static void timing(NSString *dir) {
  const int N = 64;
  NSMutableArray *ars = [NSMutableArray array];
  for (int i = 0; i < 64; i++) {
    id<MTL4Archive> a = make_archive([dir stringByAppendingPathComponent:[NSString stringWithFormat:@"t%02d.mtl4archive", i]],
                                     i == 0 ? 0 : 1000 + i * 4, i == 0 ? N : 4);
    if (a) { [ars addObject:a]; [a release]; }
  }
  id<MTL4Archive> main0 = ars[0];
  double t0 = now_ms();
  for (int n = 0; n < N; n++) { MTL4RenderPipelineDescriptor *d = desc(n); [[main0 newRenderPipelineStateWithDescriptor:d error:nil] release]; [d release]; }
  const double hitArchive = (now_ms() - t0) / N;
  MTL4CompilerTaskOptions *opt = [[MTL4CompilerTaskOptions alloc] init];
  opt.lookupArchives = @[main0];
  id<MTL4Compiler> c = compiler(nil);
  t0 = now_ms();
  for (int n = 0; n < N; n++) { MTL4RenderPipelineDescriptor *d = desc(n); [[c newRenderPipelineStateWithDescriptor:d compilerTaskOptions:opt error:nil] release]; [d release]; }
  const double hitLookup = (now_ms() - t0) / N;
  printf("TIME hit via MTL4Archive %.3f ms, via compiler+lookupArchives %.3f ms\n", hitArchive, hitLookup);
  const int ks[] = {1, 8, 64};
  for (int ki = 0; ki < 3; ki++) {
    const int k = ks[ki];
    t0 = now_ms();
    for (int m = 0; m < 16; m++) {
      MTL4RenderPipelineDescriptor *d = desc(5000 + m);
      for (int i = 0; i < k; i++) [[ars[i] newRenderPipelineStateWithDescriptor:d error:nil] release];
      [d release];
    }
    printf("TIME miss through %d archives %.3f ms\n", k, (now_ms() - t0) / 16);
  }
  [opt release]; [c release];
}

/* phys_footprint: what macOS memory pressure counts for this process */
static double footprint_mb(void) {
  task_vm_info_data_t vi;
  mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) != KERN_SUCCESS)
    return -1.0;
  return (double)vi.phys_footprint / 1048576.0;
}

/* mem <direct|compiler|probe> <workdir>: the memory of 800 live PSOs whose pipelines are all in one
 * archive (bridge bundle, the psoab RSS question). A large fragment function (unrolled ALU), so
 * per-PSO copies of the binaries would show:
 *   direct    [MTL4Archive newRenderPipelineStateWithDescriptor:] (archive first's hit)
 *   compiler  a serializer-less compiler with lookupArchives = the archive (the base path)
 *   probe     the direct lookup as a probe, its PSO released at once, then the compiler (option B)
 * One mode per process; the first call only builds the archive. Prints the footprint before and
 * after creating the PSOs, and the delta per PSO. */
static const char *kBigSrcHead =
  "#include <metal_stdlib>\nusing namespace metal;\n"
  "vertex float4 vs(uint id [[vertex_id]]) { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2 - 1, 0.5, 1); }\n"
  "fragment float4 fs(float4 pos [[position]]) { float4 a = pos * 0.001; float4 b = a.yzwx;\n";

static bool big_library(void) {
  NSMutableString *src = [NSMutableString stringWithUTF8String:kBigSrcHead];
  for (int i = 0; i < 300; i++)
    [src appendFormat:@"  a = fma(a, b, float4(%d.5)); b = sin(a) * %d.25 + b.wzyx;\n", i % 97, i % 13];
  [src appendString:@"  return a * 0.0001 + b * 0.0001; }\n"];
  NSError *err = nil;
  [g_lib release];
  g_lib = [g_dev newLibraryWithSource:src options:nil error:&err];
  if (!g_lib) { printf("FAIL big library: %s\n", err.localizedDescription.UTF8String); g_fail++; }
  return g_lib != nil;
}

/* peak <chunk> <workdir>: a consolidation's memory peak. The 800 PSOs of the mem archive are
 * rebuilt through a serializer-attached compiler with lookupArchives = that archive (lookups,
 * as pso_consolidate does), flushing the serializer every <chunk> PSOs (0: once, at the end).
 * A thread samples phys_footprint every 5 ms; prints the peak above the start, the time and the
 * files written. */
static _Atomic int g_sampling;
static double g_peak_mb;
static void *sampler(void *arg) {
  (void)arg;
  while (atomic_load(&g_sampling)) {
    const double f = footprint_mb();
    if (f > g_peak_mb) g_peak_mb = f;
    usleep(5000);
  }
  return NULL;
}

static void peak(int chunk, NSString *dir) {
  const int N = 800;
  if (!big_library()) return;
  NSError *err = nil;
  NSString *path = [dir stringByAppendingPathComponent:@"mem.mtl4archive"];
  id<MTL4Archive> ar = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:path] error:&err];
  if (!ar) { printf("FAIL no %s (run mem once first)\n", path.UTF8String); g_fail++; return; }
  MTL4CompilerTaskOptions *opt = [[MTL4CompilerTaskOptions alloc] init];
  opt.lookupArchives = @[ar];
  id<MTL4PipelineDataSetSerializer> ser = serializer();
  id<MTL4Compiler> c = compiler(ser);
  const double f0 = footprint_mb();
  g_peak_mb = f0;
  atomic_store(&g_sampling, 1);
  pthread_t th;
  pthread_create(&th, NULL, sampler, NULL);
  const double t0 = now_ms();
  int files = 0;
  for (int n = 0; n < N; n++) {
    @autoreleasepool {
      MTL4RenderPipelineDescriptor *d = desc(n);
      [[c newRenderPipelineStateWithDescriptor:d compilerTaskOptions:opt error:nil] release];
      [d release];
      if ((chunk && (n + 1) % chunk == 0) || n + 1 == N) {
        NSString *out = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"c%d-%03d.mtl4archive", chunk, files++]];
        if (![ser serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:out] error:&err]) {
          printf("FAIL serialize %s: %s\n", out.UTF8String, err.localizedDescription.UTF8String); g_fail++;
        }
      }
    }
  }
  const double t1 = now_ms();
  atomic_store(&g_sampling, 0);
  pthread_join(th, NULL);
  printf("PEAK chunk %d: %d files, %.0f ms, footprint start %.1f MB, peak +%.1f MB\n",
         chunk, files, t1 - t0, f0, g_peak_mb - f0);
  [opt release]; [c release]; [ser release]; [ar release];
}

/* archload <workdir>: the startup cost per archive: newArchiveWithURL of every c<chunk>-*.mtl4archive
 * there and one lookup in each. */
static void archload(NSString *dir) {
  if (!big_library()) return;
  NSArray *names = [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil]
    filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH 'c' AND SELF ENDSWITH '.mtl4archive'"]];
  NSMutableArray *ars = [NSMutableArray array];
  const double t0 = now_ms();
  for (NSString *n in names) {
    id<MTL4Archive> a = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:n]] error:nil];
    if (a) { [ars addObject:a]; [a release]; }
  }
  const double t1 = now_ms();
  MTL4RenderPipelineDescriptor *d = desc(0);
  for (id<MTL4Archive> a in ars) [[a newRenderPipelineStateWithDescriptor:d error:nil] release];
  [d release];
  printf("ARCHLOAD %lu archives: open %.2f ms each, one lookup each %.2f ms\n", (unsigned long)ars.count,
         ars.count ? (t1 - t0) / ars.count : 0.0, ars.count ? (now_ms() - t1) / ars.count : 0.0);
}

static void mem(const char *mode, NSString *dir) {
  const int N = 800;
  if (!big_library()) return;
  NSError *err = nil;
  NSString *path = [dir stringByAppendingPathComponent:@"mem.mtl4archive"];
  if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
    /* First call: only build the archive (its compiles would weigh on this process's footprint) */
    id<MTL4Archive> built = make_archive(path, 0, N);
    printf("MEM archive of %d PSOs built: %s\n", N, built ? path.UTF8String : "FAILED");
    if (!built) g_fail++;
    [built release];
    return;
  }
  id<MTL4Archive> ar = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:path] error:&err];
  if (!ar) { printf("FAIL archive\n"); g_fail++; return; }
  NSDictionary *at = [[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
  MTL4CompilerTaskOptions *opt = [[MTL4CompilerTaskOptions alloc] init];
  opt.lookupArchives = @[ar];
  id<MTL4Compiler> c = compiler(nil);
  const double f0 = footprint_mb();
  NSMutableArray *live = [NSMutableArray arrayWithCapacity:N];
  int hits = 0;
  for (int n = 0; n < N; n++) {
    @autoreleasepool {
      MTL4RenderPipelineDescriptor *d = desc(n);
      id<MTLRenderPipelineState> p = nil;
      if (!strcmp(mode, "direct")) {
        p = [ar newRenderPipelineStateWithDescriptor:d error:nil];
      } else {
        if (!strcmp(mode, "probe")) {
          id<MTLRenderPipelineState> q = [ar newRenderPipelineStateWithDescriptor:d error:nil];
          hits += q != nil;
          [q release];
        }
        p = [c newRenderPipelineStateWithDescriptor:d compilerTaskOptions:opt error:nil];
      }
      if (p) { [live addObject:p]; [p release]; }
      [d release];
    }
  }
  const double f1 = footprint_mb();
  printf("MEM %s: archive %.1f MB, %lu live PSOs (%d probe hits), footprint %.1f -> %.1f MB, +%.1f KB per PSO\n",
         mode, [at[NSFileSize] doubleValue] / 1048576.0, (unsigned long)live.count, hits, f0, f1,
         (f1 - f0) * 1024.0 / (live.count ? live.count : 1));
  CHECK(live.count == (NSUInteger)N, "%s: %lu of %d PSOs", mode, (unsigned long)live.count, N);
  [opt release]; [c release]; [ar release];
}

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 3) { fprintf(stderr, "usage: archfirst <check|time|mem MODE> <workdir>\n"); return 2; }
    g_dev = MTLCreateSystemDefaultDevice();
    NSError *err = nil;
    g_lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:nil error:&err];
    if (!strcmp(argv[1], "peak")) {
      if (argc < 4) { fprintf(stderr, "usage: archfirst peak <chunk|0> <workdir>\n"); return 2; }
      peak(atoi(argv[2]), [NSString stringWithUTF8String:argv[3]]);
      printf("archfirst peak %s: %s (%d failures)\n", argv[2], g_fail ? "FAIL" : "ok", g_fail);
      return g_fail ? 1 : 0;
    }
    if (!strcmp(argv[1], "archload")) {
      archload([NSString stringWithUTF8String:argv[2]]);
      return g_fail ? 1 : 0;
    }
    if (!strcmp(argv[1], "mem")) {
      if (argc < 4) { fprintf(stderr, "usage: archfirst mem <direct|compiler|probe> <workdir>\n"); return 2; }
      mem(argv[2], [NSString stringWithUTF8String:argv[3]]);
      printf("archfirst mem %s: %s (%d failures)\n", argv[2], g_fail ? "FAIL" : "ok", g_fail);
      return g_fail ? 1 : 0;
    }
    NSString *dir = [NSString stringWithUTF8String:argv[2]];
    if (!strcmp(argv[1], "check")) check(dir); else timing(dir);
    printf("archfirst %s: %s (%d failures)\n", argv[1], g_fail ? "FAIL" : "ok", g_fail);
  }
  return g_fail ? 1 : 0;
}
