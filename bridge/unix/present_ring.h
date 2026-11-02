/* D9MT_PROFILE's SCREEN line (d9mt #153): one presented-time stream per presenter, written by
 * MTLDrawable presented handlers (Metal threads, any number at once) and read by its owner
 * (the app thread, one reader).
 *
 * Plain C11 atomics and no Objective-C, so the native test (test/native/present_ring_test.cpp,
 * also under ThreadSanitizer) drives exactly this code.
 *
 * Slot ownership. Present number i (from 0) goes to slot i % D9MT_PRESENT_RING. A slot's tag is
 * 0 (never used), busy(i) while writer i owns it, or pub(i) once writer i has published. Writer i
 * takes the slot with a compare-and-swap from a tag of an older present that is published (or 0);
 * it gives up, writing nothing, if the slot is busy or already holds a newer present. So a wrapping
 * writer never overlaps an unfinished one, and an older writer never overwrites a newer record.
 *
 * Loss accounting. Only the reader counts losses, once per present number, as it moves its cursor:
 *   - pub(r): copied; kept only if the tag is still pub(r) after the copy (otherwise a newer
 *     writer took the slot during the copy: lost);
 *   - busy(r): writer r is still writing: stop here until the next drain;
 *   - a newer present's tag: r was overwritten or its writer gave up: lost;
 *   - an older present's tag (or 0): writer r hasn't taken the slot yet, or gave up on a busy
 *     older writer. The reader can't tell which unless no writer is running: it skips r as lost
 *     only when every claimed writer has finished (finished == claimed); otherwise it stops
 *     until the next drain. A cursor never stays stuck: once writers go quiet it moves on, and a
 *     newer present in that slot also moves it on.
 *   - the reader more than a ring behind the writers: everything older than the last ring's worth
 *     is lost at once.
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
  _Atomic uint64_t issued;   /* tracked presents (seq source, from 1) */
  _Atomic uint64_t claimed;  /* handlers that have run (or are running): present numbers handed out */
  _Atomic uint64_t finished; /* handlers that have finished (published or given up) */
  uint64_t read;             /* the reader's cursor (one reader) */
  uint64_t lost;             /* counted by the reader only */
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
/* the present number a tag is for; tag 0 (never used) gives UINT64_MAX ("older than all") */
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

/* At present time: the next seq (from 1), and a reference for the handler. */
static inline uint64_t d9mt_present_ring_issue(struct d9mt_present_ring *r) {
  d9mt_present_ring_ref(r);
  return atomic_fetch_add_explicit(&r->issued, 1, memory_order_relaxed) + 1;
}

/* The handler, in steps (separate so the test can delay and interleave writers):
 * take a present number; own its slot (1: owned, 0: gave up); then fill + publish; always
 * finish. */
static inline uint64_t d9mt_present_ring_take(struct d9mt_present_ring *r) {
  return atomic_fetch_add_explicit(&r->claimed, 1, memory_order_acq_rel);
}

static inline int d9mt_present_ring_own(struct d9mt_present_ring *r, uint64_t i,
                                        struct d9mt_present_slot **slot_out) {
  struct d9mt_present_slot *s = &r->slots[i % D9MT_PRESENT_RING];
  uint64_t t = atomic_load_explicit(&s->tag, memory_order_acquire);
  for (;;) {
    if (t & D9MT_TAG_BUSY)
      return 0; /* another writer owns it */
    if (t != 0 && d9mt_tag_index(t) >= i)
      return 0; /* a newer (or this very) present is already there */
    if (atomic_compare_exchange_weak_explicit(&s->tag, &t, d9mt_tag_busy(i),
                                              memory_order_acq_rel, memory_order_acquire)) {
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

static inline void d9mt_present_ring_publish(struct d9mt_present_slot *s, uint64_t i) {
  atomic_store_explicit(&s->tag, d9mt_tag_pub(i), memory_order_release);
}

/* Every handler ends here (published or given up), then drops its reference. */
static inline int d9mt_present_ring_finish(struct d9mt_present_ring *r) {
  atomic_fetch_add_explicit(&r->finished, 1, memory_order_release);
  return d9mt_present_ring_unref(r);
}

/* The whole handler. */
static inline void d9mt_present_ring_record(struct d9mt_present_ring *r, uint64_t seq,
                                            double submit_s, double presented_s) {
  const uint64_t i = d9mt_present_ring_take(r);
  struct d9mt_present_slot *s;
  if (d9mt_present_ring_own(r, i, &s)) {
    d9mt_present_ring_fill(s, seq, submit_s, presented_s);
    d9mt_present_ring_publish(s, i);
  }
  d9mt_present_ring_finish(r);
}

/* The reader (one at a time): up to max records in present order; *issued and *lost are this
 * stream's totals. */
static inline uint32_t d9mt_present_ring_drain(struct d9mt_present_ring *r,
                                               struct d9mt_present_rec *out, uint32_t max,
                                               uint64_t *issued, uint64_t *lost) {
  uint32_t n = 0;
  /* finished first, then claimed: equal means no writer was running at that moment */
  const uint64_t fin = atomic_load_explicit(&r->finished, memory_order_acquire);
  const uint64_t end = atomic_load_explicit(&r->claimed, memory_order_acquire);
  const int quiet = fin == end;
  if (end - r->read > D9MT_PRESENT_RING) {
    r->lost += end - D9MT_PRESENT_RING - r->read;
    r->read = end - D9MT_PRESENT_RING;
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
      r->read++;
      if (atomic_load_explicit(&s->tag, memory_order_relaxed) == want)
        out[n++] = rec;
      else
        r->lost++; /* a newer writer took the slot during the copy */
      continue;
    }
    if (t == d9mt_tag_busy(r->read))
      break; /* writer r is still writing */
    if (t != 0 && d9mt_tag_index(t) > r->read) {
      r->lost++; /* overwritten, or its writer gave up on a newer present */
      r->read++;
      continue;
    }
    /* an older present's tag (or none): writer r hasn't taken it yet, or gave up */
    if (!quiet)
      break;
    r->lost++;
    r->read++;
  }
  if (issued)
    *issued = atomic_load_explicit(&r->issued, memory_order_relaxed);
  if (lost)
    *lost = r->lost;
  return n;
}

#endif
