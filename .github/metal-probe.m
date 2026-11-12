// metal-probe.m: what this machine's default Metal device can do, for the Checks workflow's
// metal-probe job (.github/workflows/checks.yml). One part per run, so a part that crashes
// doesn't take the others with it:
//   metal-probe device     the device, its limits and its GPU families
//   metal-probe metal4     the Metal 4 objects the bridge creates at start-up
//                          (bridge/unix/winemetal_unix.c), and one empty commit
//   metal-probe textures   the widest 2D texture the device creates
// Build: clang -framework Foundation -framework Metal metal-probe.m -o metal-probe
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Metal/MTL4CommandQueue.h>
#import <Metal/MTL4CommandAllocator.h>
#import <Metal/MTL4CommandBuffer.h>
#import <Metal/MTL4Compiler.h>
#include <stdio.h>
#include <string.h>

static void say(NSString *format, ...) NS_FORMAT_FUNCTION(1, 2);
static void say(NSString *format, ...) {
  va_list args;
  va_start(args, format);
  NSString *line = [[NSString alloc] initWithFormat:format arguments:args];
  va_end(args);
  printf("%s\n", line.UTF8String);
  fflush(stdout);
  [line release];
}

static void device_part(id<MTLDevice> d) {
  NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
  say(@"devices: %lu", (unsigned long)all.count);
  say(@"default device: %@ (registryID 0x%llx)", d.name, (unsigned long long)d.registryID);
  say(@"lowPower %d, removable %d, headless %d, unifiedMemory %d", (int)d.lowPower, (int)d.removable,
      (int)d.headless, (int)d.hasUnifiedMemory);
  say(@"maxBufferLength %lu MB, recommendedMaxWorkingSetSize %llu MB",
      (unsigned long)(d.maxBufferLength >> 20), (unsigned long long)(d.recommendedMaxWorkingSetSize >> 20));
  // The numbers of MTLGPUFamily, as the bridge passes them, so this builds with any SDK.
  say(@"supportsFamily: Apple7 %d, Apple8 %d, Apple9 %d, Apple10 %d, Mac2 %d, Metal3 %d, Metal4 %d",
      (int)[d supportsFamily:(MTLGPUFamily)1007], (int)[d supportsFamily:(MTLGPUFamily)1008],
      (int)[d supportsFamily:(MTLGPUFamily)1009], (int)[d supportsFamily:(MTLGPUFamily)1010],
      (int)[d supportsFamily:(MTLGPUFamily)2002], (int)[d supportsFamily:(MTLGPUFamily)5001],
      (int)[d supportsFamily:(MTLGPUFamily)5002]);
}

static void metal4_part(id<MTLDevice> d) {
  if (@available(macOS 26.0, *)) {
    id<MTL4CommandQueue> queue = [d newMTL4CommandQueue];
    say(@"newMTL4CommandQueue: %@", queue ? @"ok" : @"nil");
    id<MTL4CommandAllocator> allocator = [d newCommandAllocator];
    say(@"newCommandAllocator: %@", allocator ? @"ok" : @"nil");
    id<MTL4CommandBuffer> buffer = [d newCommandBuffer];
    say(@"newCommandBuffer (MTL4): %@", buffer ? @"ok" : @"nil");
    NSError *error = nil;
    MTL4CompilerDescriptor *descriptor = [[MTL4CompilerDescriptor alloc] init];
    id<MTL4Compiler> compiler = [d newCompilerWithDescriptor:descriptor error:&error];
    say(@"newCompilerWithDescriptor: %@", compiler ? @"ok" : error ? error.localizedDescription : @"nil");
    if (!queue || !allocator || !buffer) {
      say(@"Metal 4: not usable (an object is missing)");
      return;
    }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSString *feedback = nil;
    [buffer beginCommandBufferWithAllocator:allocator];
    [buffer endCommandBuffer];
    MTL4CommitOptions *options = [[MTL4CommitOptions alloc] init];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
      feedback = [(fb.error ? fb.error.localizedDescription : @"no error") copy];
      dispatch_semaphore_signal(done);
    }];
    [queue commit:&buffer count:1 options:options];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)))
      say(@"empty commit: no feedback within 10 s");
    else
      say(@"empty commit: feedback, %@", feedback);
    say(@"Metal 4: %@", compiler ? @"the start-up objects exist" : @"no compiler");
  } else {
    say(@"Metal 4: needs macOS 26");
  }
}

static void textures_part(id<MTLDevice> d) {
  static const NSUInteger widths[] = { 4096, 8192, 16384, 32768 };
  NSUInteger widest = 0;
  for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
    say(@"2D texture %lu x 1: trying", (unsigned long)widths[i]);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                  width:widths[i]
                                                                                 height:1
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texture = [d newTextureWithDescriptor:td];
    if (!texture)
      break;
    widest = widths[i];
  }
  say(@"max texture size: %lu (the widest 2D texture created, of 4096 to 32768)", (unsigned long)widest);
}

int main(int argc, char **argv) {
  @autoreleasepool {
    const char *part = argc > 1 ? argv[1] : "";
    id<MTLDevice> d = MTLCreateSystemDefaultDevice();
    if (!d) {
      say(@"%s: no default Metal device", part);
      return 0;
    }
    if (!strcmp(part, "device"))
      device_part(d);
    else if (!strcmp(part, "metal4"))
      metal4_part(d);
    else if (!strcmp(part, "textures"))
      textures_part(d);
    else {
      fprintf(stderr, "usage: metal-probe device|metal4|textures\n");
      return 2;
    }
  }
  return 0;
}
