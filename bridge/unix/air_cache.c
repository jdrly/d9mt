/* d9mt's AIR metallib cache in front of the d9mtair compile (moved out of
 * winemetal_unix.c): the cache key, the stored blob layout and this thunk all
 * shape the bytes a compile returns, so this file's object is part of the
 * cache's build identity (scripts/build-bridge.sh), together with cache.c and
 * the codegen. */
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef __cdecl
#define __cdecl
#endif
#include "../../airlib/d9mtair.h"

extern int d9mtair_compile(struct d9mt_air_compile_params *p);

/* Persistent AIR metallib cache. The SM3->AIR codegen (LLVM, Rosetta-
 * translated) otherwise reruns for every shader and every VS/PS linkage on
 * every launch. Key = SHA-256(build identity, ABI, flags, spec data, entry,
 * IR bytes); store = the DXMT-derived sqlite cache (cache.c), one table per
 * build identity (tables of other identities are dropped on open, so a codegen
 * change never reads stale metallibs and the file does not grow without bound).
 * Every failure degrades to a plain compile.
 *   D9MT_AIR_CACHE=0            disable
 *   D9MT_AIR_CACHE=<unix path>  database file (relative = under the macOS
 *                               user cache dir, default d9mt/air-cache.sqlite3) */
@interface CacheReader : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (dispatch_data_t)get:(NSData *)key;
@end
@interface CacheWriter : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (void)set:(NSData *)key value:(dispatch_data_t)value;
@end
void d9mt_cache_drop_other_versions(NSString *path, uint64_t version);

#define D9MT_AIR_CACHE_MAGIC   0x414d3944u
#define D9MT_AIR_CACHE_HDR     12u

static pthread_mutex_t g_air_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             g_air_cache_state;
static CacheReader    *g_air_cache_reader;
static CacheWriter    *g_air_cache_writer;
static uint8_t         g_air_build_id[16];

/* The build identity: d9mt_air_codegen_id, generated and linked by
 * scripts/build-bridge.sh, "D9MT-AIR-CODEGEN-ID " + the hex SHA-256 of every
 * object that decides what a compile returns, this file's included (S1,
 * docs/bridge-bundle.md). The Metal thunks in winemetal_unix.c are not among
 * them, so changing those keeps every user's AIR cache. A build without it (not
 * made by the script) falls back to the LC_UUID of the image holding the AIR
 * codegen (this .so), which every rebuild changes, else the file's
 * inode/size/mtime. */
extern const char d9mt_air_codegen_id[] __attribute__((weak));

static bool
air_codegen_identity(uint8_t out[16]) {
  static const char prefix[] = "D9MT-AIR-CODEGEN-ID ";
  if (!d9mt_air_codegen_id || strncmp(d9mt_air_codegen_id, prefix, sizeof(prefix) - 1u))
    return false;
  const char *hex = d9mt_air_codegen_id + sizeof(prefix) - 1u;
  for (unsigned i = 0u; i < 16u; i++) {
    unsigned v;
    if (sscanf(hex + 2u * i, "%2x", &v) != 1)
      return false;
    out[i] = (uint8_t)v;
  }
  return true;
}

static bool
air_build_identity(uint8_t out[16]) {
  if (air_codegen_identity(out))
    return true;
  Dl_info dl;
  if (!dladdr((const void *)d9mtair_compile, &dl) || !dl.dli_fbase)
    return false;
  const struct mach_header_64 *mh = (const struct mach_header_64 *)dl.dli_fbase;
  if (mh->magic == MH_MAGIC_64) {
    const uint8_t *cmd = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0u; i < mh->ncmds; i++) {
      const struct load_command *lc = (const struct load_command *)cmd;
      if (lc->cmd == LC_UUID) {
        memcpy(out, ((const struct uuid_command *)lc)->uuid, 16u);
        return true;
      }
      cmd += lc->cmdsize;
    }
  }
  struct stat st;
  if (!dl.dli_fname || stat(dl.dli_fname, &st))
    return false;
  const uint64_t id[2] = { (uint64_t)st.st_ino ^ ((uint64_t)st.st_size << 32),
                           (uint64_t)st.st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st.st_mtimespec.tv_nsec };
  memcpy(out, id, 16u);
  return true;
}

static void
air_cache_init(void) {
  const char *env = getenv("D9MT_AIR_CACHE");
  g_air_cache_state = -1;
  if (env && (!*env || !strcmp(env, "0")))
    return;
  if (!air_build_identity(g_air_build_id)) {
    fprintf(stderr, "d9mt: AIR cache disabled (no build identity)\n");
    return; /* never risk stale metallibs */
  }

  uint64_t version;
  memcpy(&version, g_air_build_id, sizeof(version));
  version = (version ^ ((uint64_t)D9MT_AIR_ABI_VERSION << 56)) & 0x7fffffffffffffffull;

  NSString *path = env ? [NSString stringWithUTF8String:env] : @"d9mt/air-cache.sqlite3";
  if (!path)
    return;
  d9mt_cache_drop_other_versions(path, version);
  g_air_cache_writer = [[CacheWriter alloc] initWithPath:path version:version];
  if (g_air_cache_writer)
    g_air_cache_reader = [[CacheReader alloc] initWithPath:path version:version];
  if (g_air_cache_writer && g_air_cache_reader) {
    g_air_cache_state = 1;
    fprintf(stderr, "d9mt: AIR cache %s (table cache_%llu)\n", path.UTF8String, (unsigned long long)version);
  } else {
    [g_air_cache_writer release];
    [g_air_cache_reader release];
    g_air_cache_writer = nil;
    g_air_cache_reader = nil;
    fprintf(stderr, "d9mt: AIR cache disabled (could not open %s)\n", path.UTF8String);
  }
}

static void
air_cache_key(const struct d9mt_air_compile_params *p, unsigned char out[CC_SHA256_DIGEST_LENGTH]) {
  CC_SHA256_CTX c;
  CC_SHA256_Init(&c);
  CC_SHA256_Update(&c, g_air_build_id, sizeof(g_air_build_id));
  const uint64_t hdr[2] = { D9MT_AIR_ABI_VERSION, ((uint64_t)p->flags << 32) | p->spec_count };
  CC_SHA256_Update(&c, hdr, sizeof(hdr));
  if (p->spec_count && p->spec_data)
    CC_SHA256_Update(&c, (const void *)(uintptr_t)p->spec_data, (CC_LONG)(p->spec_count * 2u * sizeof(uint32_t)));
  const char *entry = p->entry_name ? (const char *)(uintptr_t)p->entry_name : "main";
  CC_SHA256_Update(&c, entry, (CC_LONG)strlen(entry) + 1u);
  CC_SHA256_Update(&c, (const void *)(uintptr_t)p->ir_data, (CC_LONG)p->ir_size);
  CC_SHA256_Final(out, &c);
}

static bool
air_cache_unpack(struct d9mt_air_compile_params *p, const uint8_t *b, size_t len) {
  uint32_t hdr[3];
  if (!b || len < D9MT_AIR_CACHE_HDR)
    return false;
  memcpy(hdr, b, sizeof(hdr));
  if (hdr[0] != D9MT_AIR_CACHE_MAGIC || D9MT_AIR_CACHE_HDR + (size_t)hdr[1] + hdr[2] != len || !hdr[1])
    return false;
  const uint8_t *ml = b + D9MT_AIR_CACHE_HDR, *rf = ml + hdr[1];
  if (p->pe_metallib_buffer || p->pe_reflection_buffer) {
    if (hdr[1] > p->pe_metallib_capacity || hdr[2] > p->pe_reflection_capacity)
      return false; /* let the real compile report BUFFER_TOO_SMALL */
    memcpy((void *)(uintptr_t)p->pe_metallib_buffer, ml, hdr[1]);
    if (hdr[2])
      memcpy((void *)(uintptr_t)p->pe_reflection_buffer, rf, hdr[2]);
    p->out_metallib = 0u;
    p->out_reflection = 0u;
  } else {
    /* d9mtair_free() is plain free(), so malloc'd outputs are compatible. */
    void *m = malloc(hdr[1]), *r = malloc(hdr[2] ? hdr[2] : 1u);
    if (!m || !r) {
      free(m);
      free(r);
      return false;
    }
    memcpy(m, ml, hdr[1]);
    memcpy(r, rf, hdr[2]);
    p->out_metallib = (uint64_t)(uintptr_t)m;
    p->out_reflection = (uint64_t)(uintptr_t)r;
  }
  p->out_size = hdr[1];
  p->out_reflection_size = hdr[2];
  p->ret_status = D9MT_AIR_OK;
  return true;
}

/* The d9mtair compile thunk with the cache in front (winemetal_unix.c's
 * thunk_D9MTAirCompile). Always 0: the result is in p->ret_status. */
int
d9mt_air_cache_compile(void *args) {
  struct d9mt_air_compile_params *p = args;
  unsigned char key[CC_SHA256_DIGEST_LENGTH];
  NSData *k = nil;

  pthread_mutex_lock(&g_air_cache_mutex);
  if (!g_air_cache_state)
    air_cache_init();
  const bool cache = g_air_cache_state > 0;
  pthread_mutex_unlock(&g_air_cache_mutex);

  if (cache && p->ir_data && p->ir_size) {
    air_cache_key(p, key);
    k = [[NSData alloc] initWithBytesNoCopy:key length:sizeof(key) freeWhenDone:NO];
    pthread_mutex_lock(&g_air_cache_mutex);
    dispatch_data_t hit = [g_air_cache_reader get:k];
    pthread_mutex_unlock(&g_air_cache_mutex);
    if (hit) {
      const void *bytes = NULL;
      size_t len = 0u;
      dispatch_data_t flat = dispatch_data_create_map(hit, &bytes, &len);
      const bool ok = air_cache_unpack(p, bytes, len);
      dispatch_release(flat);
      dispatch_release(hit);
      if (ok) {
        [k release];
        return 0;
      }
    }
  }

  d9mtair_compile(args);

  if (k && p->ret_status == D9MT_AIR_OK && p->out_size
   && p->out_size <= 0xffffffffu && p->out_reflection_size <= 0xffffffffu) {
    const bool pe = p->pe_metallib_buffer || p->pe_reflection_buffer;
    const void *ml = (const void *)(uintptr_t)(pe ? p->pe_metallib_buffer : p->out_metallib);
    const void *rf = (const void *)(uintptr_t)(pe ? p->pe_reflection_buffer : p->out_reflection);
    const size_t total = D9MT_AIR_CACHE_HDR + p->out_size + p->out_reflection_size;
    uint8_t *blob = malloc(total);
    if (blob && ml && (rf || !p->out_reflection_size)) {
      const uint32_t hdr[3] = { D9MT_AIR_CACHE_MAGIC, (uint32_t)p->out_size, (uint32_t)p->out_reflection_size };
      memcpy(blob, hdr, sizeof(hdr));
      memcpy(blob + D9MT_AIR_CACHE_HDR, ml, p->out_size);
      if (p->out_reflection_size)
        memcpy(blob + D9MT_AIR_CACHE_HDR + p->out_size, rf, p->out_reflection_size);
      dispatch_data_t v = dispatch_data_create(blob, total, NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
      pthread_mutex_lock(&g_air_cache_mutex);
      [g_air_cache_writer set:k value:v];
      pthread_mutex_unlock(&g_air_cache_mutex);
      dispatch_release(v);
    } else {
      free(blob);
    }
  }
  [k release];
  return 0;
}
