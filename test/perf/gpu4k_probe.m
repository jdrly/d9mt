// gpu4k_probe: synthetic 4K multi-pass Metal 4 frame, GPU time per frame by
// variant (docs/gpu-4k.md). Native macOS, no Wine, no d9mt:
//   clang -fobjc-arc -O2 -framework Metal -framework Foundation gpu4k_probe.m -o /tmp/gpu4k_probe
//   gpu4k_probe <variant> [frames]
// Variants (comma-separated flags):
//   bar=all|frag|none   end-of-pass producer barrier: after Fragment before All
//                       (d9mt today), before Fragment|Blit|Dispatch, or none
//   pfv=1|0             textures with MTLTextureUsagePixelFormatView (d9mt today) or not
//   ds=store|dontcare   dontcare: every pass but the last clears depth/stencil
//                       instead of loading it and doesn't store it (upper bound)
//   split=N             end the command buffer (commit) every N passes (0: one per frame)
//   sample=1            each pass samples the previous pass's color target (dependency)
//   halves=1            each pass ends half-way and resumes with Load (a pass split:
//                       implicit flush, mid-pass clear or copy)
//   present=copy|draw   then copy the last color target to a "drawable" texture in its
//                       own command buffer: compute-encoder copy (d9mt today) or a
//                       fullscreen draw with load DontCare
// 20 passes of 3840x2160 BGRA8 + Depth32Float_Stencil8, each one fullscreen
// triangle plus 125 draws of 120 small triangles (vertex-heavy). Prints the
// median GPU span per frame (MTL4CommitFeedback GPUStartTime..GPUEndTime) after
// 30 warm-up frames; frames run back to back, one in flight.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kSrc =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VO { float4 pos [[position]]; float4 col; };\n"
"static uint hash(uint x){ x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }\n"
// fullscreen triangle
"vertex VO vs_full(uint vid [[vertex_id]]) {\n"
"  float2 p = float2((vid << 1) & 2, vid & 2);\n"
"  VO o; o.pos = float4(p * 2.0 - 1.0, 0.5, 1.0); o.col = float4(p, 0.3, 1); return o; }\n"
// many small triangles with some vertex ALU (stands in for skinned geometry)
"vertex VO vs_small(uint vid [[vertex_id]], uint iid [[instance_id]]) {\n"
"  uint t = vid / 3u, c = vid % 3u; uint h = hash(t * 7919u + iid * 104729u);\n"
"  float2 base = float2(h & 0xffffu, h >> 16) / 65535.0 * 2.0 - 1.0;\n"
"  float a = float(c) * 2.094395;\n"
"  float3 v = float3(cos(a), sin(a), 0.0) * 0.004;\n"
"  float4x4 m = float4x4(1.0);\n"
"  for (int i = 0; i < 12; i++) { float s = sin(float(i) + base.x), co = cos(float(i) + base.y);\n"
"    m = m * float4x4(float4(co, s, 0, 0), float4(-s, co, 0, 0), float4(0, 0, 1, 0), float4(0, 0, 0, 1)); }\n"
"  float4 p = m * float4(v, 1.0);\n"
"  VO o; o.pos = float4(base + p.xy, 0.4, 1.0); o.col = float4(p.xyz * 0.5 + 0.5, 1); return o; }\n"
"fragment float4 fs_full(VO in [[stage_in]]) {\n"
"  float4 c = in.col; for (int i = 0; i < 6; i++) c = fract(c * 1.37 + 0.11); return c; }\n"
"fragment float4 fs_sample(VO in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
"  constexpr sampler s(filter::linear); float2 uv = in.pos.xy / float2(t.get_width(), t.get_height());\n"
"  float4 c = t.sample(s, uv); for (int i = 0; i < 6; i++) c = fract(c * 1.37 + 0.11); return c; }\n"
"fragment float4 fs_small(VO in [[stage_in]]) { return in.col; }\n"
"fragment float4 fs_copy(VO in [[stage_in]], texture2d<float> t [[texture(0)]]) { return t.read(uint2(in.pos.xy)); }\n";

static double median(double *v, int n) {
  for (int i = 1; i < n; i++) for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
  return n ? v[n / 2] : 0;
}

int main(int argc, char **argv) {
  @autoreleasepool {
    const char *variant = argc > 1 ? argv[1] : "";
    int frames = argc > 2 ? atoi(argv[2]) : 200;
    int bar = 0, pfv = 1, dsDontCare = 0, split = 0, sample = 0;
    if (strstr(variant, "bar=frag")) bar = 1;
    if (strstr(variant, "bar=none")) bar = 2;
    if (strstr(variant, "pfv=0")) pfv = 0;
    if (strstr(variant, "ds=dontcare")) dsDontCare = 1;
    const char *sp = strstr(variant, "split="); if (sp) split = atoi(sp + 6);
    if (strstr(variant, "sample=1")) sample = 1;
    int present = strstr(variant, "present=copy") ? 1 : strstr(variant, "present=draw") ? 2 : 0;
    int halves = strstr(variant, "halves=1") != NULL;

    const int W = 3840, H = 2160, PASSES = 20, DRAWS = 125, TRIS = 120;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSrc) options:nil error:&err];
    if (!lib) { fprintf(stderr, "lib: %s\n", err.description.UTF8String); return 2; }

    MTLPixelFormat cf = MTLPixelFormatBGRA8Unorm, df = MTLPixelFormatDepth32Float_Stencil8;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:cf width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | (pfv ? MTLTextureUsagePixelFormatView : 0);
    id<MTLTexture> col[2] = { [dev newTextureWithDescriptor:td], [dev newTextureWithDescriptor:td] };
    td.pixelFormat = df;
    td.usage = MTLTextureUsageRenderTarget | (pfv ? MTLTextureUsagePixelFormatView : 0);
    id<MTLTexture> depth = [dev newTextureWithDescriptor:td];
    td.pixelFormat = cf; td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> pres = [dev newTextureWithDescriptor:td];  // stands in for the drawable

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.colorAttachments[0].pixelFormat = cf;
    pd.depthAttachmentPixelFormat = df;
    pd.stencilAttachmentPixelFormat = df;
    pd.vertexFunction = [lib newFunctionWithName:@"vs_full"];
    pd.fragmentFunction = [lib newFunctionWithName:sample ? @"fs_sample" : @"fs_full"];
    id<MTLRenderPipelineState> psoFull = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.vertexFunction = [lib newFunctionWithName:@"vs_small"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fs_small"];
    id<MTLRenderPipelineState> psoSmall = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    MTLRenderPipelineDescriptor *pc = [MTLRenderPipelineDescriptor new];
    pc.colorAttachments[0].pixelFormat = cf;
    pc.vertexFunction = [lib newFunctionWithName:@"vs_full"];
    pc.fragmentFunction = [lib newFunctionWithName:@"fs_copy"];
    id<MTLRenderPipelineState> psoCopy = [dev newRenderPipelineStateWithDescriptor:pc error:&err];
    if (!psoFull || !psoSmall || !psoCopy) { fprintf(stderr, "pso: %s\n", err.description.UTF8String); return 2; }
    MTLDepthStencilDescriptor *dsd = [MTLDepthStencilDescriptor new];
    dsd.depthCompareFunction = MTLCompareFunctionLessEqual; dsd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> dss = [dev newDepthStencilStateWithDescriptor:dsd];

    MTLResidencySetDescriptor *rsd = [MTLResidencySetDescriptor new];
    id<MTLResidencySet> rs = [dev newResidencySetWithDescriptor:rsd error:&err];
    [rs addAllocation:col[0]]; [rs addAllocation:col[1]]; [rs addAllocation:depth]; [rs addAllocation:pres];
    [rs commit]; [rs requestResidency];

    id<MTL4CommandQueue> q = [dev newMTL4CommandQueue];
    [q addResidencySet:rs];
    id<MTL4ArgumentTable> at = nil;
    if (1) {
      MTL4ArgumentTableDescriptor *atd = [MTL4ArgumentTableDescriptor new];
      atd.maxTextureBindCount = 1;
      at = [dev newArgumentTableWithDescriptor:atd error:&err];
    }
    enum { NALLOC = 3 };
    id<MTL4CommandAllocator> allocs[NALLOC];
    for (int i = 0; i < NALLOC; i++) allocs[i] = [dev newCommandAllocator];
    id<MTLSharedEvent> ev = [dev newSharedEvent];
    uint64_t evv = 0;

    double *gpu = calloc(frames, sizeof(double));
    __block double fStart = 0, fEnd = 0;
    int warm = 30;
    for (int f = 0; f < frames + warm; f++) {
      @autoreleasepool {
        if (evv >= NALLOC) [ev waitUntilSignaledValue:evv - NALLOC + 1 timeoutMS:10000];
        id<MTL4CommandAllocator> al = allocs[f % NALLOC];
        [al reset];
        __block double s0 = 1e30, e0 = 0; __block int pending = 0;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        id<MTL4CommandBuffer> cb = [dev newCommandBuffer];
        [cb beginCommandBufferWithAllocator:al];
        int cbs = 0;
        void (^commit)(id<MTL4CommandBuffer>, BOOL) = ^(id<MTL4CommandBuffer> c, BOOL last) {
          [c endCommandBuffer];
          MTL4CommitOptions *o = [MTL4CommitOptions new];
          __sync_fetch_and_add(&pending, 1);
          [o addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
            @synchronized (done) { if (fb.GPUStartTime < s0) s0 = fb.GPUStartTime; if (fb.GPUEndTime > e0) e0 = fb.GPUEndTime; }
            if (__sync_sub_and_fetch(&pending, 1) == 0 && last) dispatch_semaphore_signal(done);
          }];
          id<MTL4CommandBuffer> arr[1] = { c };
          [q commit:arr count:1 options:o];
        };
        for (int p = 0; p < PASSES; p++) {
          MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
          id<MTLTexture> dst = col[p & 1], src = col[(p + 1) & 1];
          rp.colorAttachments[0].texture = dst;
          rp.colorAttachments[0].loadAction = p == 0 ? MTLLoadActionClear : MTLLoadActionLoad;
          rp.colorAttachments[0].storeAction = MTLStoreActionStore;
          rp.depthAttachment.texture = depth;
          rp.depthAttachment.loadAction = p == 0 || dsDontCare ? MTLLoadActionClear : MTLLoadActionLoad;
          rp.depthAttachment.clearDepth = 1.0;
          rp.depthAttachment.storeAction = dsDontCare && p != PASSES - 1 ? MTLStoreActionDontCare : MTLStoreActionStore;
          rp.stencilAttachment.texture = depth;
          rp.stencilAttachment.loadAction = rp.depthAttachment.loadAction;
          rp.stencilAttachment.storeAction = rp.depthAttachment.storeAction;
          id<MTL4RenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
          [enc setDepthStencilState:dss];
          [enc setRenderPipelineState:psoFull];
          if (sample) {
            [at setTexture:src.gpuResourceID atIndex:0];
            [enc setArgumentTable:at atStages:MTLRenderStageFragment];
          }
          [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
          [enc setRenderPipelineState:psoSmall];
          for (int d = 0; d < DRAWS; d++) {
            if (halves && d == DRAWS / 2) {
              // the pass ends mid-way (implicit flush / clear / copy) and resumes with Load
              if (bar == 0)
                [enc barrierAfterStages:MTLStageFragment beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
              else if (bar == 1)
                [enc barrierAfterStages:MTLStageFragment beforeQueueStages:MTLStageFragment | MTLStageBlit | MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
              [enc endEncoding];
              rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
              rp.depthAttachment.loadAction = MTLLoadActionLoad;
              rp.stencilAttachment.loadAction = MTLLoadActionLoad;
              enc = [cb renderCommandEncoderWithDescriptor:rp];
              [enc setDepthStencilState:dss];
              [enc setRenderPipelineState:psoSmall];
            }
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:TRIS * 3 instanceCount:1 baseInstance:p * DRAWS + d];
          }
          if (bar == 0)
            [enc barrierAfterStages:MTLStageFragment beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
          else if (bar == 1)
            [enc barrierAfterStages:MTLStageFragment beforeQueueStages:MTLStageFragment | MTLStageBlit | MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
          [enc endEncoding];
          if (split && (p + 1) % split == 0 && p != PASSES - 1) {
            commit(cb, NO); cbs++;
            cb = [dev newCommandBuffer];
            [cb beginCommandBufferWithAllocator:al];
          }
        }
        if (present == 1) {
          commit(cb, NO); cbs++;
          cb = [dev newCommandBuffer];
          [cb beginCommandBufferWithAllocator:al];
          id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
          [ce copyFromTexture:col[(PASSES - 1) & 1] toTexture:pres];
          [ce barrierAfterStages:MTLStageBlit | MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
          [ce endEncoding];
        } else if (present == 2) {
          commit(cb, NO); cbs++;
          cb = [dev newCommandBuffer];
          [cb beginCommandBufferWithAllocator:al];
          MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
          rp.colorAttachments[0].texture = pres;
          rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
          rp.colorAttachments[0].storeAction = MTLStoreActionStore;
          id<MTL4RenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
          [enc setRenderPipelineState:psoCopy];
          [at setTexture:col[(PASSES - 1) & 1].gpuResourceID atIndex:0];
          [enc setArgumentTable:at atStages:MTLRenderStageFragment];
          [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
          [enc endEncoding];
        }
        commit(cb, YES); cbs++;
        [q signalEvent:ev value:++evv];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        // wait for every feedback of this frame (the last one signals when pending hits 0)
        while (pending) usleep(50);
        if (f >= warm) gpu[f - warm] = (e0 - s0) * 1e3;
        (void)fStart; (void)fEnd; (void)cbs;
      }
    }
    double med = median(gpu, frames);
    printf("%-44s frames=%d  GPU span/frame median %.3f ms  p10 %.3f  p90 %.3f\n", variant, frames, med,
           gpu[frames / 10], gpu[frames * 9 / 10]);
    return 0;
  }
}
