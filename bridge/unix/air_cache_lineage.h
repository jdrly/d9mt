/*
 * AIR cache lineages (ADR-0006). SPDX-License-Identifier: LGPL-2.1-or-later
 * (see bridge/LICENSE, bridge/COPYING.LIB). Jan Drlý and contributors (d9mt), 2026-10.
 */
#ifndef D9MT_AIR_CACHE_LINEAGE_H
#define D9MT_AIR_CACHE_LINEAGE_H

#include <stdbool.h>
#include <stdint.h>

/* The prefix of the AIR codegen id string scripts/build-bridge.sh generates
 * (scripts/air-codegen-id.py): "D9MT-AIR-CODEGEN-ID <64 hex digits>". */
#define D9MT_AIR_CODEGEN_PREFIX "D9MT-AIR-CODEGEN-ID "
extern const char d9mt_air_codegen_id[];

/* One AIR codegen id that is proven to produce the same metallibs as an
 * earlier build, byte for byte, so it keeps that build's cache identity and
 * with it the players' cached metallibs. */
struct d9mt_air_lineage {
  const char *codegen_id;  /* the exact AIR codegen id (64 lowercase hex digits) */
  uint8_t     id[16];      /* the cache identity it uses instead of its own */
  uint64_t    adopt_table; /* an older table taken over once (cache_<n>), 0 = none */
  uint32_t    adopt_target; /* ... only by a process with this AIR target term T
                            * (d9mtair_air_target(); 0.1.8's encoding, bridge/unix/air_cache.c) */
  const char *name;        /* for the log */
};

extern const struct d9mt_air_lineage d9mt_air_lineages[];
extern const unsigned d9mt_air_lineage_count;

/* The effective AIR identity of this build: the cache identity of the lineage
 * the compiled-in AIR codegen id maps to, else the id's first 16 bytes. Pure:
 * it opens no database, so the AIR cache and the PSO archives agree on it
 * whether or not the AIR cache is in use. False (and no identity) when the id
 * is missing or malformed, a build not made by scripts/build-bridge.sh.
 * Outside the AIR codegen id, like the lineage table (ADR-0006). */
bool d9mt_air_effective_identity(uint8_t out[16], const struct d9mt_air_lineage **lineage);

#endif
