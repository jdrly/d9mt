// unlink_lookup_test <dir> [unused|warmup]: does a compiler with lookupArchives = [an MTL4Archive
// whose file was unlinked] still build? (bridge bundle: a consolidation deletes parts that are
// still lookupArchives of the game's compiler, and of other processes'.)
//   (none)  the compiler used the archive before the unlink: fine
//   unused  only direct archive lookups before the unlink: every compile with it throws
//           NSInvalidArgumentException (convertBinaryArchives): Metal's behaviour, the reason for
//           pso_convert_lookups
//   warmup  the bridge's fix: one compile of a pipeline the archive doesn't hold, through another
//           compiler, before the unlink: fine
//   clang -arch x86_64 -O1 -fobjc-arc -fobjc-arc-exceptions bridge/tests/unlink_lookup_test.m \
//     -framework Metal -framework Foundation -o unlink_lookup_test
#import <Metal/Metal.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
static const char *kSrc = "#include <metal_stdlib>\nusing namespace metal;\n"
  "vertex float4 vs(uint id [[vertex_id]]) { return float4(float2((id << 1) & 2, id & 2) * 2 - 1, 0.5, 1); }\n"
  "fragment float4 fs() { return float4(0.75, 0.25, 0.5, 1.0); }\n";
static id<MTLDevice> dev; static id<MTLLibrary> lib;
static MTL4RenderPipelineDescriptor *desc(int n) {
  MTL4RenderPipelineDescriptor *d = [[MTL4RenderPipelineDescriptor alloc] init];
  MTL4LibraryFunctionDescriptor *v = [[MTL4LibraryFunctionDescriptor alloc] init]; v.library = lib; v.name = @"vs";
  MTL4LibraryFunctionDescriptor *f = [[MTL4LibraryFunctionDescriptor alloc] init]; f.library = lib; f.name = @"fs";
  d.vertexFunctionDescriptor = v; d.fragmentFunctionDescriptor = f;
  d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  d.colorAttachments[0].blendingState = MTL4BlendStateEnabled;
  d.colorAttachments[0].sourceRGBBlendFactor = (MTLBlendFactor)(1 + n % 11);
  d.colorAttachments[0].destinationRGBBlendFactor = (MTLBlendFactor)(1 + n / 11 % 11);
  return d;
}
int main(int argc, char **argv) {
  @autoreleasepool {
    dev = MTLCreateSystemDefaultDevice();
    lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:nil error:nil];
    NSString *path = [NSString stringWithFormat:@"%s/u.mtl4archive", argv[1]];
    MTL4PipelineDataSetSerializerDescriptor *sd = [[MTL4PipelineDataSetSerializerDescriptor alloc] init];
    sd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
    id<MTL4PipelineDataSetSerializer> ser = [dev newPipelineDataSetSerializerWithDescriptor:sd];
    MTL4CompilerDescriptor *cd = [[MTL4CompilerDescriptor alloc] init]; cd.pipelineDataSetSerializer = ser;
    id<MTL4Compiler> cap = [dev newCompilerWithDescriptor:cd error:nil];
    for (int n = 0; n < 8; n++) (void)[cap newRenderPipelineStateWithDescriptor:desc(n) compilerTaskOptions:nil error:nil];
    NSError *err = nil;
    [ser serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:path] error:&err];
    id<MTL4Archive> ar = [dev newArchiveWithURL:[NSURL fileURLWithPath:path] error:&err];
    MTL4CompilerTaskOptions *opt = [[MTL4CompilerTaskOptions alloc] init]; opt.lookupArchives = @[ar];
    id<MTL4Compiler> c = [dev newCompilerWithDescriptor:[[MTL4CompilerDescriptor alloc] init] error:nil];
    const bool unused = argc > 2;   /* "unused": unlink before the compiler ever used the archive */
    if (argc > 2 && !strcmp(argv[2], "warmup")) {
      /* the fix: one compile of a descriptor the archive doesn't hold, through another compiler,
       * with the archive in lookupArchives, before the unlink */
      MTL4CompilerTaskOptions *wo = [[MTL4CompilerTaskOptions alloc] init]; wo.lookupArchives = @[ar];
      id<MTL4Compiler> wc = [dev newCompilerWithDescriptor:[[MTL4CompilerDescriptor alloc] init] error:nil];
      id w = [wc newRenderPipelineStateWithDescriptor:desc(99) compilerTaskOptions:wo error:nil];
      printf("warm-up compile (a miss) through another compiler: %s\n", w ? "ok" : "nil");
    } else if (!unused) {
      id p = [c newRenderPipelineStateWithDescriptor:desc(1) compilerTaskOptions:opt error:nil];
      printf("before unlink: hit %s\n", p ? "ok" : "nil");
    } else {
      (void)[ar newRenderPipelineStateWithDescriptor:desc(1) error:nil];   /* archive first only */
      printf("archive used directly only, the compiler not yet\n");
    }
    unlink(path.fileSystemRepresentation);
    @try {
      id q = [c newRenderPipelineStateWithDescriptor:desc(2) compilerTaskOptions:opt error:nil];
      printf("after unlink, same options object: hit %s\n", q ? "ok" : "nil");
      MTL4CompilerTaskOptions *opt2 = [[MTL4CompilerTaskOptions alloc] init]; opt2.lookupArchives = @[ar];
      id r = [c newRenderPipelineStateWithDescriptor:desc(50) compilerTaskOptions:opt2 error:nil];
      printf("after unlink, new options object: miss built %s\n", r ? "ok" : "nil");
      id<MTL4Compiler> c2 = [dev newCompilerWithDescriptor:[[MTL4CompilerDescriptor alloc] init] error:nil];
      id s = [c2 newRenderPipelineStateWithDescriptor:desc(3) compilerTaskOptions:opt2 error:nil];
      printf("after unlink, new compiler: hit %s\n", s ? "ok" : "nil");
    } @catch (NSException *e) {
      printf("EXCEPTION after unlink: %s: %s\n", e.name.UTF8String, e.reason.UTF8String);
    }
  }
  return 0;
}
