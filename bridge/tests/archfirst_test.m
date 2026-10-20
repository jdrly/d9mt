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
//   time (needs a quiet window): hit cost via MTL4Archive vs via a compiler with
//     lookupArchives, and the miss cost with 1, 8 and 64 archives loaded.
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 3) { fprintf(stderr, "usage: archfirst <check|time> <workdir>\n"); return 2; }
    g_dev = MTLCreateSystemDefaultDevice();
    NSError *err = nil;
    g_lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:nil error:&err];
    NSString *dir = [NSString stringWithUTF8String:argv[2]];
    if (!strcmp(argv[1], "check")) check(dir); else timing(dir);
    printf("archfirst %s: %s (%d failures)\n", argv[1], g_fail ? "FAIL" : "ok", g_fail);
  }
  return g_fail ? 1 : 0;
}
