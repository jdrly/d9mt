// metallib-load: one vertex shader metallib and one pixel shader metallib of d9mt's AIR compiler
// through Metal's classic API (no Metal 4), stage by stage. Used by .github/metallib-load.sh.
//
//   clang -Wall -O1 -fobjc-arc -framework Foundation -framework Metal .github/metallib-load.m -o metallib-load
//   metallib-load <vs.metallib> <ps.metallib>
//
// Exit status, with the stage and Metal's error on stdout:
//   0  both libraries load, both have the entry point "main", a render pipeline builds from
//      them (Metal compiles the AIR here), and a command buffer that encodes one triangle with
//      it completes without an error. No pixel is read: whether the device drew anything is
//      not checked, and on a hosted runner's paravirtual device it can't be taken for granted
//      (scripts/run-air-diff.sh's comparison can't tell two different shaders apart there)
//   3  newLibraryWithData refused a library (Metal's error text follows)
//   4  a library has no function "main"
//   5  newRenderPipelineStateWithDescriptor failed
//   6  no command queue, buffer or encoder, or the command buffer didn't complete
//   2  usage, an unreadable file, or no Metal device: nothing was tested
// The constant buffers are those air/tools/air_render_test.mm binds for the compiler's shaders;
// the vertex layout comes from the vertex function's own attributes. No texture is bound and
// no pixel is judged.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static id<MTLLibrary> load(id<MTLDevice> device, const char *path, int *status) {
  NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
  if (!data) {
    printf("unreadable: %s\n", path);
    *status = 2;
    return nil;
  }
  dispatch_data_t d = dispatch_data_create(data.bytes, data.length, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError *err = nil;
  id<MTLLibrary> lib = [device newLibraryWithData:d error:&err];
  if (!lib) {
    printf("newLibraryWithData refused %s: %s\n", path, err ? err.localizedDescription.UTF8String : "(no error object)");
    *status = 3;
  }
  return lib;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    printf("usage: %s <vs.metallib> <ps.metallib>\n", argv[0]);
    return 2;
  }
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      printf("no Metal device\n");
      return 2;
    }
    int status = 0;
    id<MTLLibrary> vsLib = load(device, argv[1], &status);
    id<MTLLibrary> psLib = vsLib ? load(device, argv[2], &status) : nil;
    if (!vsLib || !psLib) return status;

    id<MTLFunction> vs = [vsLib newFunctionWithName:@"main"];
    id<MTLFunction> ps = [psLib newFunctionWithName:@"main"];
    if (!vs || !ps) {
      printf("no function \"main\" in %s\n", !vs ? argv[1] : argv[2]);
      return 4;
    }

    // Every attribute the vertex function declares reads the same float4 from one buffer: the
    // pipeline needs each of them described, and no pixel is judged. Buffer 27 is the index
    // air/tools/air_diff_test.mm uses for vertices, clear of the shaders' own buffers.
    enum { kVertexBuffer = 27 };
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (MTLVertexAttribute *a in vs.vertexAttributes) {
      if (!a.active) continue;
      vd.attributes[a.attributeIndex].format = MTLVertexFormatFloat4;
      vd.attributes[a.attributeIndex].offset = 0;
      vd.attributes[a.attributeIndex].bufferIndex = kVertexBuffer;
    }
    vd.layouts[kVertexBuffer].stride = 16;
    vd.layouts[kVertexBuffer].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vs;
    pd.fragmentFunction = ps;
    pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;  // a pixel shader may write depth (oDepth)
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) {
      printf("newRenderPipelineStateWithDescriptor failed: %s\n", err ? err.localizedDescription.UTF8String : "(no error object)");
      return 5;
    }

    float vsConstants[256 * 4] = {0};
    vsConstants[0] = vsConstants[5] = vsConstants[10] = vsConstants[15] = 1.f;
    uint8_t clipPlanes[16 + 6 * 16] = {0};
    struct { int32_t sprite; float size, sizeMin, sizeMax; } pointArgs = {0, 1.f, 0.f, 64.f};
    float psConstants[224 * 4] = {0};
    psConstants[0] = psConstants[1] = psConstants[2] = psConstants[3] = 1.f;
    struct { uint32_t compareOp, precisionBits, alphaRef; } alphaTest = {8u, 8u, 0u};
    float verts[] = {
      -1.f, -1.f, 0.5f, 1.f,
       3.f, -1.f, 0.5f, 1.f,
      -1.f,  3.f, 0.5f, 1.f,
    };
#define BUF(x) [device newBufferWithBytes:&(x) length:sizeof(x) options:MTLResourceStorageModeShared]
    id<MTLBuffer> bVsC = BUF(vsConstants), bClip = BUF(clipPlanes), bPoint = BUF(pointArgs);
    id<MTLBuffer> bPsC = BUF(psConstants), bAlpha = BUF(alphaTest), bVerts = BUF(verts);

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:64 height:64 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    td.pixelFormat = MTLPixelFormatDepth32Float;
    rp.depthAttachment.texture = [device newTextureWithDescriptor:td];
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    rp.depthAttachment.clearDepth = 1.0;

    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (!queue || !cb || !enc) {  // a message to nil would encode nothing and report no error
      printf("no %s: nothing was encoded\n", !queue ? "command queue" : !cb ? "command buffer" : "render encoder");
      return 6;
    }
    [enc setRenderPipelineState:pso];
    [enc setVertexBuffer:bVsC offset:0 atIndex:0];
    [enc setVertexBuffer:bClip offset:0 atIndex:1];
    [enc setVertexBuffer:bPoint offset:0 atIndex:2];
    [enc setVertexBuffer:bVerts offset:0 atIndex:kVertexBuffer];
    [enc setFragmentBuffer:bPsC offset:0 atIndex:0];
    [enc setFragmentBuffer:bAlpha offset:0 atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.error || cb.status != MTLCommandBufferStatusCompleted) {
      printf("the command buffer didn't complete (status %lu): %s\n", (unsigned long)cb.status,
             cb.error ? cb.error.localizedDescription.UTF8String : "no error object");
      return 6;
    }
    printf("loaded, pipeline built, command buffer completed\n");
  }
  return 0;
}
