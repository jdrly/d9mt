// Unit test for bridge/unix/pso_archive_check.c (bridge bundle part 2): the v2 .sum parser
// and the archive shape check, on crafted sums and files. No Metal, no Wine.
//   clang -O1 bridge/tests/pso_archive_check_test.c bridge/unix/pso_archive_check.c -o /tmp/pact && /tmp/pact [archive dir...]
// With archive directories, every *.mtl4archive in them must also have the shape (1).
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int d9mt_pso_archive_shape(const char *path, uint64_t size);
bool d9mt_pso_sum_parse_v2(const char *text, uint64_t *size, uint64_t *mtime_ns, char sha[65]);

static int g_fail;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); g_fail++; } } while (0)

static const char kSha[] = "1035705125661aaaa0123456789abcdef0123456789abcdef0123456789abcde";

static void test_parse(void) {
  uint64_t size, mtime;
  char sha[65], buf[160];
  snprintf(buf, sizeof(buf), "2 4096 1792447607373000000 %s", kSha);
  CHECK(d9mt_pso_sum_parse_v2(buf, &size, &mtime, sha) && size == 4096u && mtime == 1792447607373000000ull
        && !strcmp(sha, kSha), "v2 parses");
  /* the reviewer's P2-M1 cases: v1 sums that sscanf("2 %llu %llu %64s") took for v2 */
  snprintf(buf, sizeof(buf), "2048576 %s", kSha);
  CHECK(!d9mt_pso_sum_parse_v2(buf, &size, &mtime, sha), "v1 with a size starting with 2 isn't v2");
  CHECK(!d9mt_pso_sum_parse_v2("2900000 9d17080000000000000000000000000000000000000000000000000000000000",
                               &size, &mtime, sha), "v1 2900000 isn't v2");
  snprintf(buf, sizeof(buf), "2 4096 17 %s extra", kSha);
  CHECK(!d9mt_pso_sum_parse_v2(buf, &size, &mtime, sha), "a fifth token");
  snprintf(buf, sizeof(buf), "2 4096 17 %s\n", kSha);
  CHECK(!d9mt_pso_sum_parse_v2(buf, &size, &mtime, sha), "a trailing newline");
  snprintf(buf, sizeof(buf), "2  4096 17 %s", kSha);
  CHECK(!d9mt_pso_sum_parse_v2(buf, &size, &mtime, sha), "a double space");
  CHECK(!d9mt_pso_sum_parse_v2("2 4096 17 1035705125661AAAA0123456789abcdef0123456789abcdef0123456789abcd",
                               &size, &mtime, sha), "uppercase hex");
  CHECK(!d9mt_pso_sum_parse_v2("2 4096 17 abc", &size, &mtime, sha), "a short SHA");
  snprintf(buf, sizeof(buf), "2 40x6 17 %s", kSha);
  CHECK(!d9mt_pso_sum_parse_v2(buf, &size, &mtime, sha), "a non-digit size");
  CHECK(!d9mt_pso_sum_parse_v2("", &size, &mtime, sha) && !d9mt_pso_sum_parse_v2(NULL, &size, &mtime, sha), "empty");
}

/* A fat archive like Metal's: n slices of `len` bytes, slice heads and ENDT placement by kind. */
typedef struct { const char *head; uint32_t len; int endt_back; } Slice;

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static size_t craft(uint8_t *out, uint32_t magic, uint32_t n, const Slice *sl, uint32_t nfile) {
  be32(out, magic);
  be32(out + 4, n);
  uint32_t off = 8u + nfile * 20u;
  off = (off + 15u) & ~15u;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t *e = out + 8 + 20 * i;
    be32(e, 0x1000017u + i);
    be32(e + 4, 0xbu);
    be32(e + 8, off);
    be32(e + 12, sl[i].len);
    be32(e + 16, 4u);
    memcpy(out + off, sl[i].head, 4);
    memcpy(out + off + sl[i].len - sl[i].endt_back, "ENDT", 4);
    off += sl[i].len;
  }
  return off;
}

static int shape_of(const uint8_t *d, size_t n, const char *name) {
  char path[256];
  snprintf(path, sizeof(path), "%s/pact-%d-%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid(), name);
  FILE *f = fopen(path, "wb");
  fwrite(d, 1, n, f);
  fclose(f);
  int r = d9mt_pso_archive_shape(path, n);
  unlink(path);
  return r;
}

static void test_shape(void) {
  static uint8_t d[1 << 20];
  const Slice two[2] = {{"MTLB", 4096, 4}, {"\xcf\xfa\xed\xfe", 8192, 8}};
  size_t n;

  memset(d, 0x5a, sizeof(d));
  n = craft(d, 0xcbfebabeu, 2, two, 2);
  CHECK(shape_of(d, n, "good") == 1, "two slices: shape");

  /* ENDT at 0, 4, 8 and 12 of the last 16 bytes (Mach-O padding seen: 0-12) */
  for (int back = 4; back <= 16; back += 4) {
    Slice s[2] = {{"MTLB", 4096, 4}, {"\xcf\xfa\xed\xfe", 8192, back}};
    memset(d, 0x5a, sizeof(d));
    n = craft(d, 0xcbfebabeu, 2, s, 2);
    CHECK(shape_of(d, n, "endt") == 1, "ENDT anywhere in the last 16 bytes");
  }

  /* power loss after a flush: the tail never reached the disk (size kept) -> no ENDT ->
   * the SHA fallback decides (-1), and the SHA mismatches, so it is deleted */
  memset(d, 0x5a, sizeof(d));
  n = craft(d, 0xcbfebabeu, 2, two, 2);
  memset(d + n - 4096, 0, 4096);
  CHECK(shape_of(d, n, "tailzero") == -1, "tail zeroed: SHA fallback");

  memset(d, 0x5a, sizeof(d));
  n = craft(d, 0xcbfebabeu, 2, two, 2);
  CHECK(shape_of(d, n - 100, "trunc") == 0, "truncated: damage");
  CHECK(shape_of(d, n + 100, "longer") == 0, "last slice not at EOF: damage");

  memset(d, 0x5a, sizeof(d));
  n = craft(d, 0xcbfebabeu, 2, two, 2);
  be32(d + 8 + 8, (uint32_t)n + 64u);
  CHECK(shape_of(d, n, "offset") == 0, "slice outside the file: damage");

  memset(d, 0, sizeof(d));
  CHECK(shape_of(d, 65536, "zero") == -1, "zero-filled: SHA fallback");

  memset(d, 0x5a, sizeof(d));
  n = craft(d, 0xcafebabeu, 2, two, 2);
  CHECK(shape_of(d, n, "magic") == -1, "another magic: SHA fallback");

  const Slice odd[2] = {{"MTLB", 4096, 4}, {"WXYZ", 8192, 8}};
  memset(d, 0x5a, sizeof(d));
  n = craft(d, 0xcbfebabeu, 2, odd, 2);
  CHECK(shape_of(d, n, "kind") == -1, "unknown slice kind: SHA fallback");

  /* the reviewer's P2-M2 case: more slices than seen is a format nuance, not damage */
  Slice many[9];
  for (int i = 0; i < 9; i++) many[i] = (Slice){i < 8 ? "MTLB" : "\xcf\xfa\xed\xfe", 2048, 4};
  memset(d, 0x5a, sizeof(d));
  n = craft(d, 0xcbfebabeu, 9, many, 9);
  CHECK(shape_of(d, n, "nine") == 1, "nine slices: shape");
  be32(d + 4, 65u);
  CHECK(shape_of(d, n, "toomany") == -1, "65 slices: SHA fallback");
  be32(d + 4, 0u);
  CHECK(shape_of(d, n, "none") == -1, "no slices: SHA fallback");
}

static void test_real(const char *dir) {
  DIR *dd = opendir(dir);
  if (!dd) { printf("FAIL cannot open %s\n", dir); g_fail++; return; }
  int n = 0, ok = 0;
  for (struct dirent *e; (e = readdir(dd)); ) {
    size_t l = strlen(e->d_name);
    if (l < 12 || strcmp(e->d_name + l - 12, ".mtl4archive")) continue;
    char p[2048];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st)) continue;
    n++;
    if (d9mt_pso_archive_shape(p, (uint64_t)st.st_size) == 1) ok++;
    else printf("FAIL real archive without the shape: %s\n", p), g_fail++;
  }
  closedir(dd);
  printf("%s: %d/%d archives have the shape\n", dir, ok, n);
}

int main(int argc, char **argv) {
  test_parse();
  test_shape();
  for (int i = 1; i < argc; i++) test_real(argv[i]);
  printf("pso_archive_check_test: %s (%d failures)\n", g_fail ? "FAIL" : "ok", g_fail);
  return g_fail ? 1 : 0;
}
