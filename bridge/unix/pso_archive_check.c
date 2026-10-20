/* PSO archive integrity checks of winemetal.so's pipeline archives (bridge bundle part 2,
 * docs/bridge-bundle.md), pure C so bridge/tests/pso_archive_check_test.c can test them
 * on crafted files and sums. */
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The shape Metal writes (macOS 27 / Metal 4; 623 archives seen, 2 or 3
 * slices): a GPU fat container, CB FE BA BE + big-endian nfat_arch +
 * {cputype, subtype, offset, size, align} per slice; each slice a metallib
 * ("MTLB") or a Mach-O (CF FA ED FE) with an "ENDT" tag in its last 16 bytes
 * (at 0, 4, 8 or 12); the last slice ends at EOF.
 *   1 = that shape;
 *   0 = proof of damage: a slice outside the file, a short read, or the last
 *       slice not ending at EOF (truncated, or written but not flushed);
 *  -1 = anything else: another magic or slice kind, an implausible slice
 *       count, no ENDT where it's expected. That may just be a format change,
 *       so the caller checks the SHA-256 rather than deleting a good archive
 *       at every start, and it is logged once.
 * A zeroed range inside a slice passes (media rot): D9MT_PSO_VERIFY=1. */
#define D9MT_PSO_MAX_SLICES 64u

static int
shape_unknown(const char *path, const char *why) {
  static _Atomic bool logged;
  if (!atomic_exchange(&logged, true))
    fprintf(stderr, "d9mt: PSO archive of an unexpected shape (%s), checking its SHA-256: %s\n", why, path);
  return -1;
}

int
d9mt_pso_archive_shape(const char *path, uint64_t size) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  uint8_t hdr[8 + D9MT_PSO_MAX_SLICES * 20];
  int result;
  if (pread(fd, hdr, 8, 0) != 8) {
    result = 0;
  } else if (OSReadBigInt32(hdr, 0) != 0xcbfebabeu) {
    result = shape_unknown(path, "magic");
  } else {
    const uint32_t n = OSReadBigInt32(hdr, 4);
    if (n < 1u || n > D9MT_PSO_MAX_SLICES) {
      result = shape_unknown(path, "slice count");
    } else if (pread(fd, hdr + 8, n * 20u, 8) != (ssize_t)(n * 20u)) {
      result = 0;
    } else {
      uint64_t end = 0u;
      result = 1;
      for (uint32_t i = 0u; result == 1 && i < n; i++) {
        const uint64_t off = OSReadBigInt32(hdr, 8 + 20 * i + 8), len = OSReadBigInt32(hdr, 8 + 20 * i + 12);
        uint8_t head[4], tail[16];
        if (off < 8u + n * 20u || len < 16u || off + len > size
         || pread(fd, head, 4, (off_t)off) != 4 || pread(fd, tail, 16, (off_t)(off + len - 16u)) != 16)
          result = 0;
        else if (memcmp(head, "MTLB", 4) && memcmp(head, "\xcf\xfa\xed\xfe", 4))
          result = shape_unknown(path, "slice kind");
        else if (!memmem(tail, sizeof(tail), "ENDT", 4))
          result = shape_unknown(path, "no ENDT");
        if (off + len > end)
          end = off + len;
      }
      if (result == 1 && end != size)
        result = 0;
    }
  }
  close(fd);
  return result;
}

static bool
all_digits(const char *s, size_t n) {
  if (!n || n > 20u)
    return false;
  for (size_t i = 0u; i < n; i++)
    if (s[i] < '0' || s[i] > '9')
      return false;
  return true;
}

/* A v2 sum, "2 <size> <mtime ns> <SHA-256, 64 lowercase hex>", parsed exactly: four
 * tokens separated by single spaces and nothing else, so a v1 sum ("<size> <SHA-256>")
 * whose size starts with 2 can't pass for one. */
bool
d9mt_pso_sum_parse_v2(const char *text, uint64_t *size, uint64_t *mtime_ns, char sha[65]) {
  if (!text || strncmp(text, "2 ", 2))
    return false;
  const char *a = text + 2, *b = strchr(a, ' ');
  if (!b)
    return false;
  const char *c = strchr(b + 1, ' ');
  if (!c || !all_digits(a, (size_t)(b - a)) || !all_digits(b + 1, (size_t)(c - b - 1)))
    return false;
  const char *h = c + 1;
  if (strlen(h) != 64u)
    return false;
  for (unsigned i = 0u; i < 64u; i++)
    if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f')))
      return false;
  *size = strtoull(a, NULL, 10);
  *mtime_ns = strtoull(b + 1, NULL, 10);
  memcpy(sha, h, 65u);
  return true;
}
