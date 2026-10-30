/*
 * AIR cache lineages (ADR-0006). SPDX-License-Identifier: LGPL-2.1-or-later
 * (see bridge/LICENSE, bridge/COPYING.LIB). Jan Drlý and contributors (d9mt), 2026-10.
 *
 * Not part of the AIR codegen id (scripts/build-bridge.sh leaves this object
 * out of the hash), so an entry can name the id of the build it is linked
 * into. An entry needs evidence that its codegen writes the same bytes as the
 * one it continues, for every input, and it holds only for that exact id: any
 * change to the codegen gives a new id, which no entry names, so the cache
 * starts cold (never stale).
 */
#include "air_cache_lineage.h"

const struct d9mt_air_lineage d9mt_air_lineages[] = {
  /* 0.1.5.1 (d9mt f6cfa71): its winemetal.so's LC_UUID
   * 2BBB585C-5076-3157-9A6A-BAE494B00ACC was its cache identity, and its
   * table cache_<(first 8 bytes, little-endian) ^ (ABI 2 << 56)>, no VERS
   * term. It tagged every metallib VERS AIR 2.9 on every macOS; since #110
   * only macOS 27 and later do, so it is adopted only there.
   * Evidence (d9mt PR "AIR cache: key it on the codegen, keep 0.1.5.1's
   * metallibs"): this id's metallibs and reflection are byte-identical to the
   * shipped 0.1.5.1 bridge's on macOS 27. */
  { "@CODEGEN_0151@",
    { 0x2b, 0xbb, 0x58, 0x5c, 0x50, 0x76, 0x31, 0x57, 0x9a, 0x6a, 0xba, 0xe4, 0x94, 0xb0, 0x0a, 0xcc },
    6138817854601476907ull, 0x00020009u, "0.1.5.1" },
};

const unsigned d9mt_air_lineage_count = sizeof(d9mt_air_lineages) / sizeof(d9mt_air_lineages[0]);
