/*
 * AIR cache lineages (ADR-0006). SPDX-License-Identifier: LGPL-2.1-or-later
 * (see bridge/LICENSE, bridge/COPYING.LIB). Jan Drlý and contributors (d9mt), 2026-10.
 */
#ifndef D9MT_AIR_CACHE_LINEAGE_H
#define D9MT_AIR_CACHE_LINEAGE_H

#include <stdint.h>

/* One AIR codegen id that is proven to produce the same metallibs as an
 * earlier build, byte for byte, so it keeps that build's cache identity and
 * with it the players' cached metallibs. */
struct d9mt_air_lineage {
  const char *codegen_id;  /* the exact AIR codegen id (64 lowercase hex digits) */
  uint8_t     id[16];      /* the cache identity it uses instead of its own */
  uint64_t    adopt_table; /* an older table taken over once (cache_<n>), 0 = none */
  uint32_t    adopt_vers;  /* ... only by a process tagging VERS with this AIR version */
  const char *name;        /* for the log */
};

extern const struct d9mt_air_lineage d9mt_air_lineages[];
extern const unsigned d9mt_air_lineage_count;

#endif
