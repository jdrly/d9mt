/* Host tool: compare two float dumps of cosf_main.exe: count of differing
 * values, the ulp histogram, the largest difference and an example input. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t ord(uint32_t b) { return (b & 0x80000000u) ? -(int64_t)(b & 0x7fffffffu) : (int64_t)b; }

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  FILE *a = fopen(argv[1], "rb"), *b = fopen(argv[2], "rb");
  if (!a || !b) { perror("open"); return 2; }
  uint32_t x, y, i = 0, diff = 0, maxi = 0;
  int64_t maxu = 0, hist[4] = {0};
  while (fread(&x, 4, 1, a) == 1 && fread(&y, 4, 1, b) == 1) {
    int64_t u = llabs(ord(x) - ord(y));
    if (u) {
      diff++;
      hist[u >= 3 ? 3 : u]++;
      if (u > maxu) { maxu = u; maxi = i; }
    }
    i++;
  }
  uint32_t inb = maxi * 64u;
  float in;
  memcpy(&in, &inb, 4);
  printf("n=%u differ=%u (1 ulp %lld, 2 ulp %lld, >=3 ulp %lld) max=%lld ulp at x=%.9g\n",
         i, diff, (long long)hist[1], (long long)hist[2], (long long)hist[3], (long long)maxu, maxu ? in : 0.0f);
  return 0;
}
