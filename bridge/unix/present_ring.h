/* D9MT_PROFILE's SCREEN line (d9mt #153): one presented-time stream per presenter, written by
 * MTLDrawable presented handlers (Metal threads, any number at once, in any order) and read by
 * its owner (the app thread, one reader).
 *
 * Plain C11 atomics and no Objective-C, so the native test (test/native/present_ring_test.cpp,
 * also under ThreadSanitizer: scripts/test-present-ring.sh) drives exactly this code.
 *
 * Positions come from the present, not from the handler. Present seq s (from 1, numbered when
 * the drawable is presented) is position i = s - 1 and goes to slot i % D9MT_PRESENT_RING. Its
 * handler may run late or out of order; the reader still retires positions strictly in order.
 *
 * Slot ownership. A slot's tag is 0 (never used), busy(i) while the writer of position i owns
 * it, or pub(i) once it has published. The writer of i takes the slot with a compare-and-swap
 * from a tag of an older position that is published (or 0), then a release fence, then its
 * relaxed payload stores, then a release store of pub(i). It gives up, writing nothing, if the
 * slot is busy or already holds a newer position. So two writers never overlap, and an older
 * writer never overwrites a newer record. The reader copies a published slot and, after an
 * acquire fence, checks the tag again: a payload that came from a newer writer synchronizes
 * through that writer's release fence with the reader's acquire fence, so the second tag check
 * sees the newer writer's busy tag and the copy is discarded (lost), never accepted mixed.
 *
 * Retirement and loss. Only the reader counts losses, once per position, as its cursor moves:
 *   - pub(r): copied, and kept unless the slot was taken during the copy (then lost);
 *   - a newer position's tag (published or busy): r was overwritten or its writer gave up: lost;
 *   - busy(r) (writer r mid-write), or an older position's tag (or 0: r's handler hasn't
 *     written yet): unresolved. If no handler is running
 *     at all (finished == issued), it never will: lost. Otherwise the cursor holds at r; once r
 *     has held for D9MT_PRESENT_HOLD_S (in practice: unresolved at the next drain, a second
 *     later, far beyond a handler's normal delay of a few refreshes), r is lost and the cursor
 *     moves on;
 *   - more than a ring behind the presents: every older position is lost at once.
 * Accepted records therefore come out in present order, and a gap in their seq marks a lost
 * position: the reader of the records (d3d9_screen.h) computes no on-screen interval across it.
 *
 * Accounting lives in the stream: accepted and lost totals, and pending = issued - accepted - lost
 * (presents whose record hasn't been retired yet). A consumer that switches between streams
 * (one per window) keeps no per-stream baselines of its own.
 *
 * Lifetime. A stream holds a reference count: one for its owner, one per present whose handler
 * hasn't run yet. Whoever drops the last reference frees it, so a handler that runs after the
 * presenter is gone writes into memory that is still valid. */
#ifndef D9MT_PRESENT_RING_H
#define D9MT_PRESENT_RING_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#define D9MT_PRESENT_RING 1024u
#define D9MT_PRESENT_HOLD_S 0.1

/* test hook: runs in the reader between copying a published slot and re-checking its tag */
#ifndef D9MT_PRESENT_RING_AFTER_COPY
#define D9MT_PRESENT_RING_AFTER_COPY(ring, slot) ((void)0)
#endif

struct d9mt_present_slot {
  _Atomic uint64_t tag;
  _Atomic uint64_t seq;
  _Atomic double submit_s;
  _Atomic double presented_s;
};

struct d9mt_present_ring {
  _Atomic uint32_t refs;
  _Atomic uint64_t issued;   /* presents numbered so far (seq of the last one) */
  _Atomic uint64_t finished; /* handlers that have finished (published or given up) */
  /* the reader's own state (one reader) */
  uint64_t read;             /* the next position to retire */
  uint64_t accepted;         /* records retired as data */
  uint64_t lost;             /* positions retired as lost */
  uint64_t hold_pos;         /* position + 1 the cursor is holding at (0: none) */
  double hold_since_s;
  struct d9mt_present_slot slots[D9MT_PRESENT_RING];
};

struct d9mt_present_rec {
  uint64_t seq;
  double submit_s;
  double presented_s;
};

#define D9MT_TAG_BUSY 1ull
static inline uint64_t d9mt_tag_pub(uint64_t i) { return (i + 1) << 1; }
static inline uint64_t d9mt_tag_busy(uint64_t i) { return ((i + 1) << 1) | D9MT_TAG_BUSY; }
/* the position a tag is for, busy or published (tag 0, never used, is handled before this is
 * called) */
static inline uint64_t d9mt_tag_index(uint64_t t) { return (t >> 1) - 1; }

static inline struct d9mt_present_ring *d9mt_present_ring_create(void) {
  struct d9mt_present_ring *r = (struct d9mt_present_ring *)calloc(1, sizeof(*r));
  if (r)
    atomic_store(&r->refs, 1u);
  return r;
}

static inline void d9mt_present_ring_ref(struct d9mt_present_ring *r) {
  atomic_fetch_add_explicit(&r->refs, 1u, memory_order_relaxed);
}

/* returns 1 if this freed the ring */
static inline int d9mt_present_ring_unref(struct d9mt_present_ring *r) {
  if (atomic_fetch_sub_explicit(&r->refs, 1u, memory_order_acq_rel) == 1u) {
    free(r);
    return 1;
  }
  return 0;
}

/* At present time (before the drawable is presented): the present's seq (from 1; its position
 * is seq - 1), and a reference for its handler. */
static inline uint64_t d9mt_present_ring_issue(struct d9mt_present_ring *r) {
  d9mt_present_ring_ref(r);
  return atomic_fetch_add_explicit(&r->issued, 1, memory_order_acq_rel) + 1;
}

/* The handler, in steps (separate so the test can delay and interleave writers): own the
 * present's slot (1: owned, 0: gave up); then fill + publish; always finish. */
static inline int d9mt_present_ring_own(struct d9mt_present_ring *r, uint64_t seq,
                                        struct d9mt_present_slot **slot_out) {
  const uint64_t i = seq - 1;
  struct d9mt_present_slot *s = &r->slots[i % D9MT_PRESENT_RING];
  uint64_t t = atomic_load_explicit(&s->tag, memory_order_acquire);
  for (;;) {
    if (t & D9MT_TAG_BUSY)
      return 0; /* another writer owns it */
    if (t != 0 && d9mt_tag_index(t) >= i)
      return 0; /* a newer (or this very) position is already there */
    if (atomic_compare_exchange_weak_explicit(&s->tag, &t, d9mt_tag_busy(i),
                                              memory_order_acq_rel, memory_order_acquire)) {
      /* orders the busy tag before the payload stores below: a reader that sees any of them
       * sees busy(i) on its second tag check (its acquire fence pairs with this one) */
      atomic_thread_fence(memory_order_release);
      *slot_out = s;
      return 1;
    }
  }
}

static inline void d9mt_present_ring_fill(struct d9mt_present_slot *s, uint64_t seq,
                                          double submit_s, double presented_s) {
  atomic_store_explicit(&s->seq, seq, memory_order_relaxed);
  atomic_store_explicit(&s->submit_s, submit_s, memory_order_relaxed);
  atomic_store_explicit(&s->presented_s, presented_s, memory_order_relaxed);
}

static inline void d9mt_present_ring_publish(struct d9mt_present_slot *s, uint64_t seq) {
  atomic_store_explicit(&s->tag, d9mt_tag_pub(seq - 1), memory_order_release);
}

/* Every handler ends here (published or given up), then drops its reference. */
static inline int d9mt_present_ring_finish(struct d9mt_present_ring *r) {
  atomic_fetch_add_explicit(&r->finished, 1, memory_order_release);
  return d9mt_present_ring_unref(r);
}

/* The whole handler. */
static inline void d9mt_present_ring_record(struct d9mt_present_ring *r, uint64_t seq,
                                            double submit_s, double presented_s) {
  struct d9mt_present_slot *s;
  if (d9mt_present_ring_own(r, seq, &s)) {
    d9mt_present_ring_fill(s, seq, submit_s, presented_s);
    d9mt_present_ring_publish(s, seq);
  }
  d9mt_present_ring_finish(r);
}

static inline void d9mt_present_ring_lose(struct d9mt_present_ring *r) {
  r->lost++;
  r->read++;
  r->hold_pos = 0;
}

/* The reader (one at a time): up to max records, strictly in present order. now_s: the reader's
 * clock (host seconds), for the hold bound. *pending = presents not yet retired, *lost_new =
 * positions this call retired as lost. */
static inline uint32_t d9mt_present_ring_drain(struct d9mt_present_ring *r,
                                               struct d9mt_present_rec *out, uint32_t max,
                                               double now_s, uint64_t *pending, uint64_t *lost_new) {
  uint32_t n = 0;
  const uint64_t lost0 = r->lost;
  /* finished first, then issued: equal means no handler was pending at that moment */
  const uint64_t fin = atomic_load_explicit(&r->finished, memory_order_acquire);
  const uint64_t end = atomic_load_explicit(&r->issued, memory_order_acquire);
  const int quiet = fin == end;
  if (end - r->read > D9MT_PRESENT_RING) {
    r->lost += end - D9MT_PRESENT_RING - r->read;
    r->read = end - D9MT_PRESENT_RING;
    r->hold_pos = 0;
  }
  while (r->read < end && n < max) {
    struct d9mt_present_slot *s = &r->slots[r->read % D9MT_PRESENT_RING];
    const uint64_t want = d9mt_tag_pub(r->read);
    const uint64_t t = atomic_load_explicit(&s->tag, memory_order_acquire);
    if (t == want) {
      struct d9mt_present_rec rec;
      rec.seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
      rec.submit_s = atomic_load_explicit(&s->submit_s, memory_order_relaxed);
      rec.presented_s = atomic_load_explicit(&s->presented_s, memory_order_relaxed);
      atomic_thread_fence(memory_order_acquire);
      D9MT_PRESENT_RING_AFTER_COPY(r, s);
      if (atomic_load_explicit(&s->tag, memory_order_relaxed) == want) {
        out[n++] = rec;
        r->accepted++;
        r->read++;
        r->hold_pos = 0;
      } else {
        d9mt_present_ring_lose(r); /* a newer writer took the slot during the copy */
      }
      continue;
    }
    if (t != 0 && d9mt_tag_index(t) > r->read) {
      d9mt_present_ring_lose(r); /* overwritten (or being written) by a newer position, or its
                                    writer gave up on one */
      continue;
    }
    /* unresolved: writer r is mid-write (busy(r)), or hasn't taken its slot yet (an older
     * position's tag, or none). Both wait under the same hold bound. A writer still mid-write
     * when r is retired as lost keeps its slot and publishes into it later; the reader, past r,
     * never reads that record, and the next accepted seq leaves a gap that breaks the chain. */
    if (quiet) {
      d9mt_present_ring_lose(r); /* no handler pending: it never will */
      continue;
    }
    if (r->hold_pos != r->read + 1) {
      r->hold_pos = r->read + 1;
      r->hold_since_s = now_s;
      break;
    }
    if (now_s - r->hold_since_s >= D9MT_PRESENT_HOLD_S) {
      d9mt_present_ring_lose(r); /* unresolved past the bound */
      continue;
    }
    break;
  }
  if (pending)
    *pending = end - r->accepted - r->lost;
  if (lost_new)
    *lost_new = r->lost - lost0;
  return n;
}

#endif
