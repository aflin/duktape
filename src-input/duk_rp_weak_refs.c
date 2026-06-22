/*
 *  duk_rp_weak_refs.c
 *
 *  Rampart contribution: WeakRef, WeakMap, WeakSet, FinalizationRegistry
 *  (ES2021).  Gated by DUK_RP_USE_WEAK_REFS.
 *
 *  Design summary
 *  --------------
 *  All four types share the single class slot DUK_HOBJECT_CLASS_WEAK_KIND
 *  (slot 31).  Per-instance kind is stored in the hidden number prop
 *  "\xff" "wk".  Each kind is implemented as:
 *
 *    WeakRef   : "\xff" "wk_target" is a dynamic buffer of one pointer
 *                (the raw hobject* of the target), or NULL/empty when
 *                cleared.  The buffer is mark-blind, so the GC does NOT
 *                traverse the target via the WeakRef.
 *    WeakMap   : "\xff" "wk_keys"   is a dynamic buffer holding N raw
 *                                   key pointers (mark-blind).
 *                "\xff" "wk_values" is a JS Array of N values (strong).
 *    WeakSet   : "\xff" "wk_keys"   is a dynamic buffer holding N raw
 *                                   element pointers (mark-blind).
 *    FinReg    : "\xff" "wk_targets" is a dynamic buffer holding N raw
 *                                   target pointers (mark-blind).
 *                "\xff" "wk_held"   is a JS Array of N held values (strong).
 *                "\xff" "wk_tokens" is a JS Array of N unregister tokens
 *                                   (held strongly -- spec actually wants
 *                                   them weak but doing so adds a second
 *                                   weakness layer; v1 keeps tokens strong).
 *                "\xff" "wk_cb"     is the cleanup callback (strong).
 *
 *  A heap-level back-table maps target hobject* -> linked list of
 *  (holder, role) pairs.  When a target dies (refcount-zero OR found
 *  unreachable after mark), the back-table is consulted and each holder
 *  has the target removed from its weak storage.  When a holder dies,
 *  its registrations are removed from the back-table.
 *
 *  GC hooks
 *  --------
 *  1. duk_heap_refcount.c (refcount-zero path):
 *       duk_rp_weak_back_target_dying(heap, obj)
 *     Called every refcount-zero death (gated on heap->weak_back_table
 *     non-NULL).  Both target-role and holder-role cleanup happen here.
 *  2. duk_heap_markandsweep.c (after temproots, before sweep):
 *       duk_rp_weak_postmark_cleanup(heap)
 *     Walks heap_allocated; for each WEAK_KIND object, prunes weak slots
 *     whose pointee is unreachable.  Also flushes back-table entries for
 *     dead holders and dead targets discovered by the mark pass.
 *  3. duk_heap_alloc.c (heap free):
 *       duk_rp_weak_back_table_free(heap)
 *     Releases the hash table and any pending finalizer queue.
 *
 *  FinalizationRegistry callbacks
 *  ------------------------------
 *  Queued at cleanup time but not auto-fired -- the spec wants them on
 *  a "host-defined cleanup task" which rampart's embedder runs out of
 *  band.  duk_rp_weak_drain_pending_finalizers(thr) drains the queue
 *  and invokes each callback with its held value.  The embedder is
 *  responsible for calling drain at safe points (e.g. between event
 *  loop ticks).  Until embedders are updated, callbacks accumulate.
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_WEAK_REFS)

/* D6: holder-role cleanup is delegated to the object finalizer
 * (duk__weak_kind_finalizer).  Without finalizer support every WeakRef/WeakMap/
 * WeakSet/FinalizationRegistry constructor would throw at runtime AND collected
 * holders would never be cleaned.  Fail the build loudly instead of shipping a
 * silently-broken feature. */
#if !defined(DUK_USE_FINALIZER_SUPPORT)
#error "DUK_RP_USE_WEAK_REFS requires DUK_USE_FINALIZER_SUPPORT"
#endif

/* ------------------------------------------------------------------ *
 *  Subtype enum and hidden property keys                              *
 * ------------------------------------------------------------------ */

#define WK_REF   0
#define WK_MAP   1
#define WK_SET   2
#define WK_FREG  3

/* Hidden internal-key strings (start with 0xFF byte to make them
 * non-enumerable, non-discoverable from JS). */
static const char STR_KIND[]    = "\xff" "wk";
static const char STR_TARGET[]  = "\xff" "wk_target";   /* WeakRef:   buffer<ptr> */
static const char STR_KEYS[]    = "\xff" "wk_keys";     /* WeakMap/WeakSet: buffer<ptr[]> */
static const char STR_VALUES[]  = "\xff" "wk_values";   /* WeakMap: Array<any> */
static const char STR_TARGETS[] = "\xff" "wk_targets";  /* FinReg: buffer<ptr[]> */
static const char STR_HELD[]    = "\xff" "wk_held";     /* FinReg: Array<any> */
static const char STR_TOKENS[]  = "\xff" "wk_tokens";   /* FinReg: Array<any> */
static const char STR_CB[]      = "\xff" "wk_cb";       /* FinReg: callable */

/* ------------------------------------------------------------------ *
 *  Back-table data structures                                         *
 * ------------------------------------------------------------------ */

typedef struct duk_rp_wk_back_entry duk_rp_wk_back_entry;
struct duk_rp_wk_back_entry {
	duk_hobject *holder;   /* WEAK_KIND object referring to the target */
	duk_uint_t   role;     /* WK_REF / WK_MAP / WK_SET / WK_FREG */
	duk_rp_wk_back_entry *next;
};

typedef struct duk_rp_wk_back_bucket duk_rp_wk_back_bucket;
struct duk_rp_wk_back_bucket {
	duk_uintptr_t target_key;          /* (uintptr_t) hobject*       */
	duk_rp_wk_back_entry *first;       /* head of holder list        */
	duk_rp_wk_back_bucket *next;       /* chain within bucket index  */
};

#define DUK_RP_WK_INITIAL_BUCKETS 32u
/* Resize trigger: when entry_count >= bucket_count * LOAD_FACTOR_NUM /
 * LOAD_FACTOR_DEN, double the bucket array.  At 2:1 we keep average
 * chain length around 2 entries even with many entries. */
#define DUK_RP_WK_RESIZE_NUM 2
#define DUK_RP_WK_RESIZE_DEN 1
/* Cap to prevent runaway growth on adversarial loads. */
#define DUK_RP_WK_MAX_BUCKETS 65536u

typedef struct duk_rp_wk_pending duk_rp_wk_pending;
struct duk_rp_wk_pending {
	duk_hobject *cb;          /* callback function (strong)   */
	duk_tval     held;        /* held value (strong)          */
	duk_rp_wk_pending *next;
};

typedef struct duk_rp_wk_table duk_rp_wk_table;
struct duk_rp_wk_table {
	duk_rp_wk_back_bucket **buckets;
	duk_uint_t bucket_count;
	duk_uint_t entry_count;
	duk_rp_wk_pending *pending_head;
	duk_rp_wk_pending *pending_tail;
	/* Embedder notifier: invoked exactly when a pending entry is added
	 * to a previously-empty queue (the queue-non-empty transition).
	 * For libevent integration this calls event_active(drain_ev) so the
	 * drain runs at the end of the current event batch.  NULL = no
	 * notifier set; queue grows silently until cleanupSome is called. */
	void (*pending_notifier)(void *udata);
	void *pending_notifier_udata;
	duk_heap *heap;           /* convenience for allocator access */
};

/* ------------------------------------------------------------------ *
 *  Helpers                                                            *
 * ------------------------------------------------------------------ */

static duk_uint_t duk__wk_hash_ptr(duk_uintptr_t p) {
	/* Simple Knuth-style multiplicative hash; sufficient for collision
	 * avoidance at modest entry counts. */
	duk_uint_t h = (duk_uint_t) (p >> 4);
	h ^= h >> 16;
	h *= 0x85ebca6bUL;
	h ^= h >> 13;
	return h;
}

static duk_rp_wk_table *duk__wk_table_ensure(duk_heap *heap) {
	duk_rp_wk_table *t = (duk_rp_wk_table *) heap->weak_back_table;
	if (t != NULL) {
		return t;
	}
	t = (duk_rp_wk_table *) heap->alloc_func(heap->heap_udata, sizeof(*t));
	if (t == NULL) {
		return NULL;
	}
	t->bucket_count = DUK_RP_WK_INITIAL_BUCKETS;
	t->entry_count = 0;
	t->pending_head = NULL;
	t->pending_tail = NULL;
	t->pending_notifier = NULL;
	t->pending_notifier_udata = NULL;
	t->heap = heap;
	t->buckets = (duk_rp_wk_back_bucket **) heap->alloc_func(
	    heap->heap_udata, sizeof(duk_rp_wk_back_bucket *) * t->bucket_count);
	if (t->buckets == NULL) {
		heap->free_func(heap->heap_udata, t);
		return NULL;
	}
	{
		duk_uint_t i;
		for (i = 0; i < t->bucket_count; i++) {
			t->buckets[i] = NULL;
		}
	}
	heap->weak_back_table = (void *) t;
	return t;
}

static duk_rp_wk_back_bucket *duk__wk_bucket_find(duk_rp_wk_table *t,
                                                  duk_uintptr_t key) {
	duk_uint_t idx = duk__wk_hash_ptr(key) % t->bucket_count;
	duk_rp_wk_back_bucket *b = t->buckets[idx];
	while (b != NULL && b->target_key != key) {
		b = b->next;
	}
	return b;
}

/* Grow the bucket array by 2x and rehash all existing entries.  Called
 * from get_or_create when the load factor crosses the threshold.  Best
 * effort: if allocation fails we just keep the old table (slower
 * lookups but correctness preserved). */
static void duk__wk_table_resize(duk_rp_wk_table *t) {
	duk_uint_t new_count;
	duk_rp_wk_back_bucket **new_buckets;
	duk_uint_t bi;

	if (t->bucket_count >= DUK_RP_WK_MAX_BUCKETS) {
		return;
	}
	new_count = t->bucket_count * 2u;
	new_buckets = (duk_rp_wk_back_bucket **) t->heap->alloc_func(
	    t->heap->heap_udata, sizeof(duk_rp_wk_back_bucket *) * new_count);
	if (new_buckets == NULL) {
		return;  /* keep old table */
	}
	for (bi = 0; bi < new_count; bi++) {
		new_buckets[bi] = NULL;
	}
	for (bi = 0; bi < t->bucket_count; bi++) {
		duk_rp_wk_back_bucket *b = t->buckets[bi];
		while (b != NULL) {
			duk_rp_wk_back_bucket *next_b = b->next;
			duk_uint_t new_idx = duk__wk_hash_ptr(b->target_key) % new_count;
			b->next = new_buckets[new_idx];
			new_buckets[new_idx] = b;
			b = next_b;
		}
	}
	t->heap->free_func(t->heap->heap_udata, t->buckets);
	t->buckets = new_buckets;
	t->bucket_count = new_count;
}

static duk_rp_wk_back_bucket *duk__wk_bucket_get_or_create(duk_rp_wk_table *t,
                                                            duk_uintptr_t key) {
	duk_uint_t idx;
	duk_rp_wk_back_bucket *b;

	idx = duk__wk_hash_ptr(key) % t->bucket_count;
	b = t->buckets[idx];
	while (b != NULL && b->target_key != key) {
		b = b->next;
	}
	if (b != NULL) {
		return b;
	}
	b = (duk_rp_wk_back_bucket *) t->heap->alloc_func(
	    t->heap->heap_udata, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	b->target_key = key;
	b->first = NULL;
	b->next = t->buckets[idx];
	t->buckets[idx] = b;
	t->entry_count++;
	/* Maybe grow.  Threshold: entries/buckets >= NUM/DEN.  Check after
	 * insert so the new entry triggers the grow if it crosses. */
	if (t->entry_count * DUK_RP_WK_RESIZE_DEN >=
	    t->bucket_count * DUK_RP_WK_RESIZE_NUM) {
		duk__wk_table_resize(t);
	}
	return b;
}

static void duk__wk_bucket_remove(duk_rp_wk_table *t, duk_uintptr_t key) {
	duk_uint_t idx = duk__wk_hash_ptr(key) % t->bucket_count;
	duk_rp_wk_back_bucket *b = t->buckets[idx];
	duk_rp_wk_back_bucket *prev = NULL;
	while (b != NULL && b->target_key != key) {
		prev = b;
		b = b->next;
	}
	if (b == NULL) {
		return;
	}
	if (prev == NULL) {
		t->buckets[idx] = b->next;
	} else {
		prev->next = b->next;
	}
	{
		duk_rp_wk_back_entry *e = b->first;
		while (e != NULL) {
			duk_rp_wk_back_entry *next_e = e->next;
			t->heap->free_func(t->heap->heap_udata, e);
			e = next_e;
		}
	}
	t->heap->free_func(t->heap->heap_udata, b);
	t->entry_count--;
}

static duk_bool_t duk__wk_back_add(duk_heap *heap,
                                    duk_hobject *target,
                                    duk_hobject *holder,
                                    duk_uint_t role) {
	duk_rp_wk_table *t;
	duk_rp_wk_back_bucket *b;
	duk_rp_wk_back_entry *e;

	t = duk__wk_table_ensure(heap);
	if (t == NULL) {
		return 0;
	}
	b = duk__wk_bucket_get_or_create(t, (duk_uintptr_t) target);
	if (b == NULL) {
		return 0;
	}
	e = (duk_rp_wk_back_entry *) heap->alloc_func(heap->heap_udata, sizeof(*e));
	if (e == NULL) {
		return 0;
	}
	e->holder = holder;
	e->role = role;
	e->next = b->first;
	b->first = e;
	return 1;
}

static void duk__wk_back_remove_holder(duk_heap *heap,
                                        duk_hobject *target,
                                        duk_hobject *holder,
                                        duk_uint_t role) {
	duk_rp_wk_table *t = (duk_rp_wk_table *) heap->weak_back_table;
	duk_rp_wk_back_bucket *b;
	duk_rp_wk_back_entry *e, *prev_e;

	if (t == NULL) {
		return;
	}
	b = duk__wk_bucket_find(t, (duk_uintptr_t) target);
	if (b == NULL) {
		return;
	}
	prev_e = NULL;
	e = b->first;
	while (e != NULL) {
		if (e->holder == holder && e->role == role) {
			duk_rp_wk_back_entry *next_e = e->next;
			if (prev_e == NULL) {
				b->first = next_e;
			} else {
				prev_e->next = next_e;
			}
			heap->free_func(heap->heap_udata, e);
			if (b->first == NULL) {
				duk__wk_bucket_remove(t, (duk_uintptr_t) target);
			}
			return;
		}
		prev_e = e;
		e = e->next;
	}
}

/* ------------------------------------------------------------------ *
 *  Pointer-buffer helpers                                             *
 *                                                                     *
 *  Each holder stores its weak pointers in a dynamic buffer; the      *
 *  buffer's payload is N raw hobject* values laid out contiguously.   *
 *  The buffer object lives on the holder as a hidden property and is  *
 *  marked normally by the GC (it's a buffer, so the marker doesn't    *
 *  recurse into it -- which is exactly what we want).                 *
 * ------------------------------------------------------------------ */

static duk_uint_t duk__wk_ptrbuf_count(duk_context *ctx, duk_idx_t holder_idx,
                                        const char *key) {
	duk_size_t sz;
	duk_get_prop_string(ctx, holder_idx, key);
	if (!duk_is_buffer(ctx, -1)) {
		duk_pop(ctx);
		return 0;
	}
	duk_get_buffer(ctx, -1, &sz);
	duk_pop(ctx);
	return (duk_uint_t) (sz / sizeof(void *));
}

/* Push the pointer at index `i` from holder's `key` buffer onto stack;
 * returns the raw pointer too.  Returns NULL on OOB or non-buffer. */
static duk_hobject *duk__wk_ptrbuf_get(duk_context *ctx, duk_idx_t holder_idx,
                                       const char *key, duk_uint_t i) {
	void **arr;
	duk_size_t sz;
	duk_hobject *p;
	duk_get_prop_string(ctx, holder_idx, key);
	if (!duk_is_buffer(ctx, -1)) {
		duk_pop(ctx);
		return NULL;
	}
	arr = (void **) duk_get_buffer(ctx, -1, &sz);
	duk_pop(ctx);
	if ((duk_size_t) i * sizeof(void *) >= sz) {
		return NULL;
	}
	p = (duk_hobject *) arr[i];
	return p;
}

/* Append a pointer to the holder's buffer (grow by sizeof(void*)). */
static void duk__wk_ptrbuf_append(duk_context *ctx, duk_idx_t holder_idx,
                                  const char *key, duk_hobject *p) {
	void **arr;
	duk_size_t sz;
	holder_idx = duk_normalize_index(ctx, holder_idx);
	duk_get_prop_string(ctx, holder_idx, key);
	if (!duk_is_buffer(ctx, -1)) {
		duk_pop(ctx);
		duk_push_dynamic_buffer(ctx, sizeof(void *));
		arr = (void **) duk_get_buffer(ctx, -1, &sz);
		arr[0] = (void *) p;
		duk_put_prop_string(ctx, holder_idx, key);
		return;
	}
	arr = (void **) duk_get_buffer(ctx, -1, &sz);
	arr = (void **) duk_resize_buffer(ctx, -1, sz + sizeof(void *));
	arr[sz / sizeof(void *)] = (void *) p;
	duk_pop(ctx);  /* buffer */
}

/* Remove the entry at index `i` by swapping the last entry into slot i
 * and shrinking by sizeof(void*).  Returns new count. */
static duk_uint_t duk__wk_ptrbuf_remove_at(duk_context *ctx, duk_idx_t holder_idx,
                                            const char *key, duk_uint_t i) {
	void **arr;
	duk_size_t sz;
	duk_uint_t count;
	holder_idx = duk_normalize_index(ctx, holder_idx);
	duk_get_prop_string(ctx, holder_idx, key);
	if (!duk_is_buffer(ctx, -1)) {
		duk_pop(ctx);
		return 0;
	}
	arr = (void **) duk_get_buffer(ctx, -1, &sz);
	count = (duk_uint_t) (sz / sizeof(void *));
	if (i >= count) {
		duk_pop(ctx);
		return count;
	}
	if (i != count - 1) {
		arr[i] = arr[count - 1];
	}
	duk_resize_buffer(ctx, -1, sz - sizeof(void *));
	duk_pop(ctx);
	return count - 1;
}

/* Find index of pointer p in holder's key buffer; returns -1 if absent. */
static duk_int_t duk__wk_ptrbuf_find(duk_context *ctx, duk_idx_t holder_idx,
                                      const char *key, duk_hobject *p) {
	void **arr;
	duk_size_t sz;
	duk_uint_t count, i;
	duk_get_prop_string(ctx, holder_idx, key);
	if (!duk_is_buffer(ctx, -1)) {
		duk_pop(ctx);
		return -1;
	}
	arr = (void **) duk_get_buffer(ctx, -1, &sz);
	count = (duk_uint_t) (sz / sizeof(void *));
	for (i = 0; i < count; i++) {
		if (arr[i] == (void *) p) {
			duk_pop(ctx);
			return (duk_int_t) i;
		}
	}
	duk_pop(ctx);
	return -1;
}

/* ------------------------------------------------------------------ *
 *  Tval-buffer helpers (used for WeakMap values storage in v2)        *
 *                                                                     *
 *  The values are stored as raw duk_tval bytes in a dynamic buffer    *
 *  attached to the holder under STR_VALUES.  Because buffers are      *
 *  mark-blind, the mark phase does NOT reach values via this storage. *
 *  The post-mark ephemeron pass selectively marks values whose keys   *
 *  are reachable.                                                     *
 *                                                                     *
 *  Refcount discipline: each occupied slot holds one INCREF on the    *
 *  tval (heap-allocated targets only; primitives are no-ops).         *
 *  Callers MUST DECREF before overwriting or shrinking a slot.        *
 * ------------------------------------------------------------------ */

/* Push the buffer on the stack and return its base pointer cast to
 * duk_tval*.  Caller must duk_pop the buffer when done.  Returns NULL
 * on missing/non-buffer prop. */
static duk_tval *duk__wk_tvalbuf_push_and_base(duk_context *ctx,
                                                duk_idx_t holder_idx,
                                                const char *key,
                                                duk_size_t *out_sz) {
	duk_get_prop_string(ctx, holder_idx, key);
	if (!duk_is_buffer(ctx, -1)) {
		duk_pop(ctx);
		if (out_sz) *out_sz = 0;
		return NULL;
	}
	return (duk_tval *) duk_get_buffer(ctx, -1, out_sz);
}

/* Append a tval to the holder's buffer (grows by sizeof(duk_tval)).
 * Initializes the new slot to a COPY of src and INCREFs it. */
static void duk__wk_tvalbuf_append(duk_context *ctx,
                                    duk_hthread *thr,
                                    duk_idx_t holder_idx,
                                    const char *key,
                                    duk_tval *src) {
	duk_tval *arr;
	duk_size_t sz;
	duk_uint_t idx;
	holder_idx = duk_normalize_index(ctx, holder_idx);
	duk_get_prop_string(ctx, holder_idx, key);
	if (!duk_is_buffer(ctx, -1)) {
		duk_pop(ctx);
		duk_push_dynamic_buffer(ctx, sizeof(duk_tval));
		arr = (duk_tval *) duk_get_buffer(ctx, -1, &sz);
		DUK_TVAL_SET_TVAL(&arr[0], src);
		DUK_TVAL_INCREF(thr, &arr[0]);
		duk_put_prop_string(ctx, holder_idx, key);
		return;
	}
	arr = (duk_tval *) duk_get_buffer(ctx, -1, &sz);
	arr = (duk_tval *) duk_resize_buffer(ctx, -1, sz + sizeof(duk_tval));
	idx = (duk_uint_t) (sz / sizeof(duk_tval));
	DUK_TVAL_SET_TVAL(&arr[idx], src);
	DUK_TVAL_INCREF(thr, &arr[idx]);
	duk_pop(ctx);  /* buffer */
}

/* Overwrite slot i: DECREF old, copy from src, INCREF new. */
static void duk__wk_tvalbuf_set(duk_context *ctx,
                                duk_hthread *thr,
                                duk_idx_t holder_idx,
                                const char *key,
                                duk_uint_t i,
                                duk_tval *src) {
	duk_tval *arr;
	duk_size_t sz;
	arr = duk__wk_tvalbuf_push_and_base(ctx, holder_idx, key, &sz);
	if (arr == NULL) {
		return;
	}
	if ((duk_size_t) i * sizeof(duk_tval) >= sz) {
		duk_pop(ctx);
		return;
	}
	DUK_TVAL_DECREF_NORZ(thr, &arr[i]);
	DUK_TVAL_SET_TVAL(&arr[i], src);
	DUK_TVAL_INCREF(thr, &arr[i]);
	duk_pop(ctx);
}

/* Push tval at index i onto the JS stack. */
static void duk__wk_tvalbuf_push(duk_context *ctx,
                                  duk_idx_t holder_idx,
                                  const char *key,
                                  duk_uint_t i) {
	duk_tval *arr;
	duk_size_t sz;
	arr = duk__wk_tvalbuf_push_and_base(ctx, holder_idx, key, &sz);
	if (arr == NULL || (duk_size_t) i * sizeof(duk_tval) >= sz) {
		if (arr != NULL) duk_pop(ctx);
		duk_push_undefined(ctx);
		return;
	}
	duk_push_tval(ctx, &arr[i]);
	duk_remove(ctx, -2);  /* buffer */
}

/* Swap-and-shrink at index i: DECREF the removed slot (only if the
 * target is REACHABLE), copy last into slot i (if i != last), shrink
 * by sizeof(duk_tval).
 *
 * The REACHABLE guard exists because this is called from post-mark
 * cleanup where the value might be heap-allocated but unreachable --
 * sweep will free its storage on its own.  Decrementing here would
 * queue the value on refzero_list, but duk_heap_mark_and_sweep asserts
 * refzero_list==NULL on exit.  When the value IS reachable (marked by
 * ephemeron or otherwise), DECREF is needed to balance the INCREF from
 * tvalbuf_set/append. */
static void duk__wk_tvalbuf_swap_shrink_at(duk_context *ctx,
                                            duk_hthread *thr,
                                            duk_idx_t holder_idx,
                                            const char *key,
                                            duk_uint_t i) {
	duk_tval *arr;
	duk_size_t sz;
	duk_uint_t count;
	duk_bool_t safe_to_decref;
	holder_idx = duk_normalize_index(ctx, holder_idx);
	arr = duk__wk_tvalbuf_push_and_base(ctx, holder_idx, key, &sz);
	if (arr == NULL) {
		return;
	}
	count = (duk_uint_t) (sz / sizeof(duk_tval));
	if (i >= count) {
		duk_pop(ctx);
		return;
	}
	/* Skip DECREF only when we're inside mark-and-sweep AND the value
	 * is heap-allocated but not reachable.  In that case sweep will
	 * free the value's storage anyway and queueing refzero would
	 * violate the heap_mark_and_sweep refzero_list==NULL invariant.
	 * Outside mark-and-sweep (refcount path), REACHABLE is cleared
	 * on all live objects, so we always DECREF to balance the
	 * INCREF from tvalbuf_set/append. */
	safe_to_decref = 1;
	if (thr->heap->ms_running && DUK_TVAL_IS_HEAP_ALLOCATED(&arr[i])) {
		duk_heaphdr *h = DUK_TVAL_GET_HEAPHDR(&arr[i]);
		if (h != NULL && !DUK_HEAPHDR_HAS_REACHABLE(h)) {
			safe_to_decref = 0;
		}
	}
	if (safe_to_decref) {
		DUK_TVAL_DECREF_NORZ(thr, &arr[i]);
	}
	if (i != count - 1) {
		/* Move last slot into i.  Last slot's INCREF is preserved by
		 * moving the bytes (no extra INCREF/DECREF needed since we
		 * just relocate the +1 from slot last to slot i). */
		arr[i] = arr[count - 1];
	}
	duk_resize_buffer(ctx, -1, (count - 1) * sizeof(duk_tval));
	duk_pop(ctx);
}

/* Release ALL slots in the buffer (DECREF each), then set buffer
 * length to 0.  Used when the WeakMap dies. */
static void duk__wk_tvalbuf_clear(duk_context *ctx,
                                   duk_hthread *thr,
                                   duk_idx_t holder_idx,
                                   const char *key) {
	duk_tval *arr;
	duk_size_t sz;
	duk_uint_t count, i;
	holder_idx = duk_normalize_index(ctx, holder_idx);
	arr = duk__wk_tvalbuf_push_and_base(ctx, holder_idx, key, &sz);
	if (arr == NULL) {
		return;
	}
	count = (duk_uint_t) (sz / sizeof(duk_tval));
	for (i = 0; i < count; i++) {
		DUK_TVAL_DECREF_NORZ(thr, &arr[i]);
	}
	duk_resize_buffer(ctx, -1, 0);
	duk_pop(ctx);
}

/* Swap-and-shrink removal at index i from a JS Array stored at
 * holder.key.  Replaces array[i] with array[length-1], then sets
 * length=length-1.  Used in parallel with ptrbuf_remove_at so that
 * the two arrays' indices stay aligned. */
static void duk__wk_array_swap_shrink_at(duk_context *ctx, duk_idx_t holder_idx,
                                          const char *key, duk_uint_t i) {
	duk_uint_t len;
	holder_idx = duk_normalize_index(ctx, holder_idx);
	duk_get_prop_string(ctx, holder_idx, key);
	if (!duk_is_array(ctx, -1)) {
		duk_pop(ctx);
		return;
	}
	len = (duk_uint_t) duk_get_length(ctx, -1);
	if (i >= len) {
		duk_pop(ctx);
		return;
	}
	if (i != len - 1) {
		duk_get_prop_index(ctx, -1, (duk_uarridx_t) (len - 1));
		duk_put_prop_index(ctx, -2, (duk_uarridx_t) i);
	}
	duk_push_uint(ctx, len - 1);
	duk_put_prop_string(ctx, -2, "length");
	duk_pop(ctx);
}

/* ------------------------------------------------------------------ *
 *  Common helpers                                                     *
 * ------------------------------------------------------------------ */

static duk_bool_t duk__wk_is_kind(duk_context *ctx, duk_idx_t idx,
                                   duk_uint_t expected_kind) {
	duk_hobject *h;
	duk_uint_t k;
	idx = duk_normalize_index(ctx, idx);
	if (!duk_is_object(ctx, idx)) {
		return 0;
	}
	h = duk_get_hobject(ctx, idx);
	if (h == NULL ||
	    DUK_HOBJECT_GET_CLASS_NUMBER(h) != DUK_HOBJECT_CLASS_WEAK_KIND) {
		return 0;
	}
	duk_get_prop_string(ctx, idx, STR_KIND);
	k = (duk_uint_t) duk_get_uint_default(ctx, -1, 0xFFFFu);
	duk_pop(ctx);
	return k == expected_kind ? 1 : 0;
}

static void duk__wk_set_class(duk_context *ctx, duk_idx_t idx,
                               duk_uint_t kind) {
	duk_hobject *h;
	idx = duk_normalize_index(ctx, idx);
	h = duk_get_hobject(ctx, idx);
	DUK_ASSERT(h != NULL);
	DUK_HOBJECT_SET_CLASS_NUMBER(h, DUK_HOBJECT_CLASS_WEAK_KIND);
	duk_push_uint(ctx, kind);
	duk_put_prop_string(ctx, idx, STR_KIND);
}

static void duk__wk_throw_typeerror_target(duk_hthread *thr,
                                           const char *who) {
	(void) duk_type_error((duk_context *) thr,
	                      "%s argument must be an object", who);
}

/* ------------------------------------------------------------------ *
 *  Finalizer attached to every WEAK_KIND instance                     *
 *                                                                     *
 *  Mark-and-sweep can free a WeakXxx via cycle collection without     *
 *  going through the refcount path -- so the refzero hook in          *
 *  duk_heap_refcount.c may not fire.  A finalizer guarantees cleanup  *
 *  in BOTH paths: it queues on finalize_list during refzero (running  *
 *  inline) and during sweep (running after each cycle).  Inside, we   *
 *  do back-table holder cleanup and (for WeakMap) value DECREFs.      *
 * ------------------------------------------------------------------ */

/* Forward decl needed because the finalizer calls it and the function
 * itself is defined later (after the JS-method bodies). */
static void duk__wk_holder_unregister_all(duk_heap *heap, duk_hobject *holder);

static duk_ret_t duk__weak_kind_finalizer(duk_context *ctx) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_hobject *obj = duk_get_hobject(ctx, 0);
	if (obj == NULL) {
		return 0;
	}
	if (DUK_HOBJECT_GET_CLASS_NUMBER(obj) != DUK_HOBJECT_CLASS_WEAK_KIND) {
		return 0;
	}
	/* duk__wk_holder_unregister_all removes back_table entries for any
	 * targets this holder references AND (for WK_MAP) clears the values
	 * tval buffer.  Idempotent: second call sees empty buffers/lists. */
	duk__wk_holder_unregister_all(thr->heap, obj);
	return 0;
}

static void duk__wk_attach_finalizer(duk_context *ctx, duk_idx_t obj_idx) {
	obj_idx = duk_normalize_index(ctx, obj_idx);
	duk_push_c_function(ctx, duk__weak_kind_finalizer, 1);
	duk_set_finalizer(ctx, obj_idx);
}

/* ------------------------------------------------------------------ *
 *  WeakRef                                                            *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__wkref_ctor(duk_context *ctx) {
	duk_hobject *target;
	duk_hobject *self;
	duk_hthread *thr = (duk_hthread *) ctx;

	if (!duk_is_constructor_call(ctx)) {
		return duk_type_error(ctx,
		    "WeakRef constructor requires 'new'");
	}
	if (!duk_is_object(ctx, 0)) {
		duk__wk_throw_typeerror_target(thr, "WeakRef");
	}
	target = duk_get_hobject(ctx, 0);
	if (target == NULL) {
		duk__wk_throw_typeerror_target(thr, "WeakRef");
	}

	duk_push_this(ctx);
	self = duk_get_hobject(ctx, -1);
	duk__wk_set_class(ctx, -1, WK_REF);

	/* Store the target raw pointer in a dynamic buffer (mark-blind). */
	{
		void **slot;
		duk_push_dynamic_buffer(ctx, sizeof(void *));
		slot = (void **) duk_get_buffer(ctx, -1, NULL);
		slot[0] = (void *) target;
		duk_put_prop_string(ctx, -2, STR_TARGET);
	}

	if (!duk__wk_back_add(thr->heap, target, self, WK_REF)) {
		return duk_range_error(ctx, "WeakRef: out of memory");
	}

	duk__wk_attach_finalizer(ctx, -1);
	duk_pop(ctx);  /* this */
	return 0;
}

static duk_ret_t duk__wkref_deref(duk_context *ctx) {
	duk_hobject *target;
	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_REF)) {
		return duk_type_error(ctx,
		    "WeakRef.prototype.deref called on non-WeakRef");
	}
	target = duk__wk_ptrbuf_get(ctx, -1, STR_TARGET, 0);
	duk_pop(ctx);
	if (target == NULL) {
		duk_push_undefined(ctx);
	} else {
		duk_push_hobject(ctx, target);
	}
	return 1;
}

/* ------------------------------------------------------------------ *
 *  WeakMap                                                            *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__wkmap_ctor(duk_context *ctx) {
	duk_hthread *thr = (duk_hthread *) ctx;

	if (!duk_is_constructor_call(ctx)) {
		return duk_type_error(ctx,
		    "WeakMap constructor requires 'new'");
	}
	duk_push_this(ctx);
	duk__wk_set_class(ctx, -1, WK_MAP);
	/* Initialize empty keys buffer (ptrs, mark-blind) and empty values
	 * buffer (duk_tval, mark-blind).  Mark-blindness on values is what
	 * gives ephemeron semantics: values are only marked transitively
	 * via the post-mark ephemeron pass when their key is reachable. */
	duk_push_dynamic_buffer(ctx, 0);
	duk_put_prop_string(ctx, -2, STR_KEYS);
	duk_push_dynamic_buffer(ctx, 0);
	duk_put_prop_string(ctx, -2, STR_VALUES);
	duk__wk_attach_finalizer(ctx, -1);
	duk_pop(ctx);

	/* Spec: optional iterable argument -- read [key,value] pairs and
	 * insert each via internal Set. */
	if (duk_get_top(ctx) >= 1 && !duk_is_null_or_undefined(ctx, 0)) {
		duk_idx_t this_idx, set_idx, src_idx;
		duk_push_this(ctx);
		this_idx = duk_get_top(ctx) - 1;
		duk_get_prop_string(ctx, this_idx, "set");
		set_idx = duk_get_top(ctx) - 1;
		duk_dup(ctx, 0);
		src_idx = duk_get_top(ctx) - 1;
		duk_enum(ctx, src_idx, DUK_ENUM_OWN_PROPERTIES_ONLY |
		                        DUK_ENUM_ARRAY_INDICES_ONLY |
		                        DUK_ENUM_SORT_ARRAY_INDICES);
		while (duk_next(ctx, -1, 1)) {
			/* TOS: entry, TOS-1: key */
			duk_idx_t entry_idx = duk_get_top(ctx) - 1;
			if (duk_is_object(ctx, entry_idx)) {
				duk_dup(ctx, set_idx);
				duk_dup(ctx, this_idx);
				duk_get_prop_index(ctx, entry_idx, 0);
				duk_get_prop_index(ctx, entry_idx, 1);
				duk_call_method(ctx, 2);
				duk_pop(ctx);  /* result */
			}
			duk_pop_2(ctx);  /* entry, key */
		}
		duk_pop_n(ctx, 4);  /* enum, src, set, this */
		(void) thr;
	}
	return 0;
}

static duk_ret_t duk__wkmap_get(duk_context *ctx) {
	duk_hobject *k;
	duk_int_t idx;
	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_MAP)) {
		return duk_type_error(ctx, "WeakMap.prototype.get on non-WeakMap");
	}
	if (!duk_is_object(ctx, 0)) {
		duk_push_undefined(ctx);
		return 1;
	}
	k = duk_get_hobject(ctx, 0);
	idx = duk__wk_ptrbuf_find(ctx, -1, STR_KEYS, k);
	if (idx < 0) {
		duk_push_undefined(ctx);
		return 1;
	}
	duk__wk_tvalbuf_push(ctx, -1, STR_VALUES, (duk_uint_t) idx);
	return 1;
}

static duk_ret_t duk__wkmap_set(duk_context *ctx) {
	duk_hobject *k;
	duk_hobject *self;
	duk_int_t idx;
	duk_hthread *thr = (duk_hthread *) ctx;

	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_MAP)) {
		return duk_type_error(ctx, "WeakMap.prototype.set on non-WeakMap");
	}
	if (!duk_is_object(ctx, 0)) {
		duk__wk_throw_typeerror_target(thr, "WeakMap.set key");
	}
	k = duk_get_hobject(ctx, 0);
	self = duk_get_hobject(ctx, -1);
	idx = duk__wk_ptrbuf_find(ctx, -1, STR_KEYS, k);
	if (idx >= 0) {
		/* D5: copy the value tval onto the C stack (AFTER ptrbuf_find's internal
		 * pushes) so a value-stack realloc can't dangle a raw tval pointer. */
		duk_tval tv_copy = *duk_get_tval(ctx, 1);
		duk__wk_tvalbuf_set(ctx, thr, -1, STR_VALUES, (duk_uint_t) idx, &tv_copy);
	} else {
		/* D4: register the back-table entry FIRST.  If it fails (OOM) nothing has
		 * been appended to KEYS/VALUES yet, so no raw key pointer is left in the
		 * mark-blind buffer without a back-table entry (which on key death would
		 * never be pruned and could later match a reused address). */
		if (!duk__wk_back_add(thr->heap, k, self, WK_MAP)) {
			return duk_range_error(ctx, "WeakMap.set: out of memory");
		}
		duk__wk_ptrbuf_append(ctx, -1, STR_KEYS, k);
		{
			duk_tval tv_copy = *duk_get_tval(ctx, 1); /* D5 */
			duk__wk_tvalbuf_append(ctx, thr, -1, STR_VALUES, &tv_copy);
		}
	}
	return 1;
}

static duk_ret_t duk__wkmap_has(duk_context *ctx) {
	duk_hobject *k;
	duk_int_t idx;
	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_MAP)) {
		return duk_type_error(ctx, "WeakMap.prototype.has on non-WeakMap");
	}
	if (!duk_is_object(ctx, 0)) {
		duk_push_boolean(ctx, 0);
		return 1;
	}
	k = duk_get_hobject(ctx, 0);
	idx = duk__wk_ptrbuf_find(ctx, -1, STR_KEYS, k);
	duk_push_boolean(ctx, idx >= 0);
	return 1;
}

static duk_ret_t duk__wkmap_delete(duk_context *ctx) {
	duk_hobject *k;
	duk_hobject *self;
	duk_int_t idx;
	duk_hthread *thr = (duk_hthread *) ctx;

	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_MAP)) {
		return duk_type_error(ctx, "WeakMap.prototype.delete on non-WeakMap");
	}
	if (!duk_is_object(ctx, 0)) {
		duk_push_boolean(ctx, 0);
		return 1;
	}
	k = duk_get_hobject(ctx, 0);
	self = duk_get_hobject(ctx, -1);
	idx = duk__wk_ptrbuf_find(ctx, -1, STR_KEYS, k);
	if (idx < 0) {
		duk_push_boolean(ctx, 0);
		return 1;
	}
	duk__wk_ptrbuf_remove_at(ctx, -1, STR_KEYS, (duk_uint_t) idx);
	duk__wk_tvalbuf_swap_shrink_at(ctx, thr, -1, STR_VALUES, (duk_uint_t) idx);
	duk__wk_back_remove_holder(thr->heap, k, self, WK_MAP);
	duk_push_boolean(ctx, 1);
	return 1;
}

/* ------------------------------------------------------------------ *
 *  WeakSet                                                            *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__wkset_ctor(duk_context *ctx) {
	duk_hthread *thr = (duk_hthread *) ctx;
	if (!duk_is_constructor_call(ctx)) {
		return duk_type_error(ctx,
		    "WeakSet constructor requires 'new'");
	}
	duk_push_this(ctx);
	duk__wk_set_class(ctx, -1, WK_SET);
	duk_push_dynamic_buffer(ctx, 0);
	duk_put_prop_string(ctx, -2, STR_KEYS);
	duk__wk_attach_finalizer(ctx, -1);
	duk_pop(ctx);
	if (duk_get_top(ctx) >= 1 && !duk_is_null_or_undefined(ctx, 0)) {
		duk_idx_t this_idx, add_idx, src_idx;
		duk_push_this(ctx);
		this_idx = duk_get_top(ctx) - 1;
		duk_get_prop_string(ctx, this_idx, "add");
		add_idx = duk_get_top(ctx) - 1;
		duk_dup(ctx, 0);
		src_idx = duk_get_top(ctx) - 1;
		duk_enum(ctx, src_idx, DUK_ENUM_OWN_PROPERTIES_ONLY |
		                        DUK_ENUM_ARRAY_INDICES_ONLY |
		                        DUK_ENUM_SORT_ARRAY_INDICES);
		while (duk_next(ctx, -1, 1)) {
			duk_idx_t elt_idx = duk_get_top(ctx) - 1;
			duk_dup(ctx, add_idx);
			duk_dup(ctx, this_idx);
			duk_dup(ctx, elt_idx);
			duk_call_method(ctx, 1);
			duk_pop(ctx);  /* result */
			duk_pop_2(ctx); /* elt, key */
		}
		duk_pop_n(ctx, 4);
		(void) thr;
	}
	return 0;
}

static duk_ret_t duk__wkset_add(duk_context *ctx) {
	duk_hobject *k;
	duk_hobject *self;
	duk_int_t idx;
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_SET)) {
		return duk_type_error(ctx, "WeakSet.prototype.add on non-WeakSet");
	}
	if (!duk_is_object(ctx, 0)) {
		duk__wk_throw_typeerror_target(thr, "WeakSet.add value");
	}
	k = duk_get_hobject(ctx, 0);
	self = duk_get_hobject(ctx, -1);
	idx = duk__wk_ptrbuf_find(ctx, -1, STR_KEYS, k);
	if (idx < 0) {
		/* D4: back-table entry first, then append (see wkmap_set). */
		if (!duk__wk_back_add(thr->heap, k, self, WK_SET)) {
			return duk_range_error(ctx, "WeakSet.add: out of memory");
		}
		duk__wk_ptrbuf_append(ctx, -1, STR_KEYS, k);
	}
	return 1;  /* this */
}

static duk_ret_t duk__wkset_has(duk_context *ctx) {
	duk_hobject *k;
	duk_int_t idx;
	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_SET)) {
		return duk_type_error(ctx, "WeakSet.prototype.has on non-WeakSet");
	}
	if (!duk_is_object(ctx, 0)) {
		duk_push_boolean(ctx, 0);
		return 1;
	}
	k = duk_get_hobject(ctx, 0);
	idx = duk__wk_ptrbuf_find(ctx, -1, STR_KEYS, k);
	duk_push_boolean(ctx, idx >= 0);
	return 1;
}

static duk_ret_t duk__wkset_delete(duk_context *ctx) {
	duk_hobject *k;
	duk_hobject *self;
	duk_int_t idx;
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_SET)) {
		return duk_type_error(ctx, "WeakSet.prototype.delete on non-WeakSet");
	}
	if (!duk_is_object(ctx, 0)) {
		duk_push_boolean(ctx, 0);
		return 1;
	}
	k = duk_get_hobject(ctx, 0);
	self = duk_get_hobject(ctx, -1);
	idx = duk__wk_ptrbuf_find(ctx, -1, STR_KEYS, k);
	if (idx < 0) {
		duk_push_boolean(ctx, 0);
		return 1;
	}
	duk__wk_ptrbuf_remove_at(ctx, -1, STR_KEYS, (duk_uint_t) idx);
	duk__wk_back_remove_holder(thr->heap, k, self, WK_SET);
	duk_push_boolean(ctx, 1);
	return 1;
}

/* ------------------------------------------------------------------ *
 *  FinalizationRegistry                                               *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__finreg_ctor(duk_context *ctx) {
	if (!duk_is_constructor_call(ctx)) {
		return duk_type_error(ctx,
		    "FinalizationRegistry constructor requires 'new'");
	}
	if (!duk_is_function(ctx, 0)) {
		return duk_type_error(ctx,
		    "FinalizationRegistry: cleanupCallback must be a function");
	}
	duk_push_this(ctx);
	duk__wk_set_class(ctx, -1, WK_FREG);
	duk_dup(ctx, 0);
	duk_put_prop_string(ctx, -2, STR_CB);
	duk_push_dynamic_buffer(ctx, 0);
	duk_put_prop_string(ctx, -2, STR_TARGETS);
	duk_push_array(ctx);
	duk_put_prop_string(ctx, -2, STR_HELD);
	duk_push_array(ctx);
	duk_put_prop_string(ctx, -2, STR_TOKENS);
	duk__wk_attach_finalizer(ctx, -1);
	duk_pop(ctx);
	return 0;
}

static duk_ret_t duk__finreg_register(duk_context *ctx) {
	duk_hobject *target;
	duk_hobject *self;
	duk_uint_t count;
	duk_hthread *thr = (duk_hthread *) ctx;

	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_FREG)) {
		return duk_type_error(ctx,
		    "FinalizationRegistry.register on non-FinalizationRegistry");
	}
	if (!duk_is_object(ctx, 0)) {
		duk__wk_throw_typeerror_target(thr, "FinalizationRegistry.register");
	}
	target = duk_get_hobject(ctx, 0);
	self = duk_get_hobject(ctx, -1);

	/* D4: register the back-table entry FIRST; if it fails (OOM) nothing has been
	 * appended to the TARGETS pointer buffer yet, so no raw target pointer is left
	 * without a back-table entry.  (HELD/TOKENS are JS arrays, not raw buffers, so
	 * a short length there is a benign undefined, not an OOB.) */
	if (!duk__wk_back_add(thr->heap, target, self, WK_FREG)) {
		return duk_range_error(ctx, "FinalizationRegistry: out of memory");
	}

	duk__wk_ptrbuf_append(ctx, -1, STR_TARGETS, target);
	count = duk__wk_ptrbuf_count(ctx, -1, STR_TARGETS);

	duk_get_prop_string(ctx, -1, STR_HELD);
	duk_dup(ctx, 1);
	duk_put_prop_index(ctx, -2, (duk_uarridx_t) (count - 1));
	duk_pop(ctx);

	duk_get_prop_string(ctx, -1, STR_TOKENS);
	if (duk_get_top(ctx) >= 3 && !duk_is_undefined(ctx, 2)) {
		duk_dup(ctx, 2);
	} else {
		duk_push_undefined(ctx);
	}
	duk_put_prop_index(ctx, -2, (duk_uarridx_t) (count - 1));
	duk_pop(ctx);

	return 0;
}

static duk_ret_t duk__finreg_unregister(duk_context *ctx) {
	duk_hobject *self;
	duk_uint_t count, i;
	duk_bool_t any = 0;
	duk_hthread *thr = (duk_hthread *) ctx;

	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_FREG)) {
		return duk_type_error(ctx,
		    "FinalizationRegistry.unregister on non-FinalizationRegistry");
	}
	if (!duk_is_object(ctx, 0)) {
		return duk_type_error(ctx,
		    "FinalizationRegistry.unregister: token must be an object");
	}
	self = duk_get_hobject(ctx, -1);

	/* Walk tokens array from end to start so splice index stays valid. */
	count = duk__wk_ptrbuf_count(ctx, -1, STR_TARGETS);
	for (i = count; i > 0; ) {
		i--;
		duk_get_prop_string(ctx, -1, STR_TOKENS);
		duk_get_prop_index(ctx, -1, (duk_uarridx_t) i);
		if (duk_strict_equals(ctx, 0, -1)) {
			duk_hobject *target = duk__wk_ptrbuf_get(ctx, -3, STR_TARGETS, i);
			duk_pop_2(ctx);  /* token, tokens-array */
			duk__wk_ptrbuf_remove_at(ctx, -1, STR_TARGETS, i);
			duk__wk_array_swap_shrink_at(ctx, -1, STR_HELD, i);
			duk__wk_array_swap_shrink_at(ctx, -1, STR_TOKENS, i);
			if (target != NULL) {
				duk__wk_back_remove_holder(thr->heap, target, self, WK_FREG);
			}
			any = 1;
		} else {
			duk_pop_2(ctx);  /* token, tokens-array */
		}
	}
	duk_pop(ctx);  /* this */
	duk_push_boolean(ctx, any);
	return 1;
}

/* JS-visible cleanupSome: drains the pending callback queue, invoking
 * either an explicit per-call callback (if provided) or the registry's
 * own cleanup callback for each pending held-value.  Spec ES2021
 * 26.2.3.2 with the cleanupSome stage-3 addition (later removed but
 * widely supported and useful for tests). */
static duk_ret_t duk__finreg_cleanup_some(duk_context *ctx) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_rp_wk_table *t;
	duk_bool_t override_cb = duk_is_function(ctx, 0);

	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_FREG)) {
		return duk_type_error(ctx,
		    "FinalizationRegistry.cleanupSome on non-FinalizationRegistry");
	}
	duk_pop(ctx);

	t = (duk_rp_wk_table *) thr->heap->weak_back_table;
	if (t == NULL) {
		return 0;
	}
	while (t->pending_head != NULL) {
		duk_rp_wk_pending *p = t->pending_head;
		t->pending_head = p->next;
		if (t->pending_head == NULL) {
			t->pending_tail = NULL;
		}
		if (override_cb) {
			duk_dup(ctx, 0);
		} else {
			duk_push_hobject(ctx, p->cb);
		}
		duk_push_tval(ctx, &p->held);
		if (duk_pcall(ctx, 1) != 0) {
			/* Swallow callback errors so one bad callback doesn't
			 * abort the drain.  Spec says they propagate; for v1 we
			 * isolate them. */
		}
		duk_pop(ctx);
		DUK_TVAL_DECREF_NORZ(thr, &p->held);
		DUK_HOBJECT_DECREF_NORZ(thr, p->cb);
		thr->heap->free_func(thr->heap->heap_udata, p);
	}
	return 0;
}

/* Register an embedder notifier called on the empty -> non-empty
 * transition of the pending callback queue.  Intended for libevent
 * integration: the callback typically does event_active(drain_ev, 0, 0)
 * so the drain runs at the end of the current event batch.
 *
 * Pass NULL to unregister.  The notifier persists for the heap's
 * lifetime or until replaced.  The udata pointer is opaque -- duktape
 * never dereferences it.
 *
 * Safe to call before any weak-ref construction: the back-table is
 * allocated lazily on first call, and the notifier is preserved.
 *
 * Public C API (declared in duktape.h when DUK_RP_USE_WEAK_REFS is on). */
DUK_EXTERNAL void duk_rp_weak_set_pending_notifier(
    duk_context *ctx,
    void (*cb)(void *udata),
    void *udata) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_rp_wk_table *t = duk__wk_table_ensure(thr->heap);
	if (t == NULL) {
		return;  /* OOM; embedder will discover via empty cleanups */
	}
	t->pending_notifier = cb;
	t->pending_notifier_udata = udata;
}

/* Explicit drain entry point for embedders.  Fires each pending FinReg
 * callback with its held value, dequeues, and DECREFs.  Errors thrown
 * inside callbacks are swallowed so a bad callback can't abort the drain.
 *
 * Public C API (declared in duktape.h when DUK_RP_USE_WEAK_REFS is on). */
DUK_EXTERNAL void duk_rp_weak_drain_pending_finalizers(duk_context *ctx) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_rp_wk_table *t;

	t = (duk_rp_wk_table *) thr->heap->weak_back_table;
	if (t == NULL || t->pending_head == NULL) {
		return;
	}
	while (t->pending_head != NULL) {
		duk_rp_wk_pending *p = t->pending_head;
		t->pending_head = p->next;
		if (t->pending_head == NULL) {
			t->pending_tail = NULL;
		}
		duk_push_hobject(ctx, p->cb);
		duk_push_tval(ctx, &p->held);
		if (duk_pcall(ctx, 1) != 0) {
			/* Swallow finalizer errors; spec says they're not observable. */
		}
		duk_pop(ctx);
		DUK_TVAL_DECREF_NORZ(thr, &p->held);
		DUK_HOBJECT_DECREF_NORZ(thr, p->cb);
		thr->heap->free_func(thr->heap->heap_udata, p);
	}
}

/* ------------------------------------------------------------------ *
 *  GC hooks: cleanup on target death                                  *
 * ------------------------------------------------------------------ */

/* Per-role cleanup when target hobject* is dying.
 *
 * Bumps the holder's refcount across the call so that pushing it on
 * the value stack does not risk an inner refzero cascade (we are
 * already inside refzero or post-mark; either way an unexpected
 * decref-to-zero of the holder mid-cleanup would corrupt the queue).
 */
static void duk__wk_cleanup_for_target(duk_heap *heap,
                                       duk_hobject *target,
                                       duk_hobject *holder,
                                       duk_uint_t role) {
	duk_context *ctx = (duk_context *) heap->heap_thread;
	duk_int_t idx;

	DUK_HEAPHDR_PREINC_REFCOUNT((duk_heaphdr *) holder);
	duk_push_hobject(ctx, holder);
	switch (role) {
	case WK_REF: {
		void **slot;
		duk_size_t sz;
		duk_get_prop_string(ctx, -1, STR_TARGET);
		if (duk_is_buffer(ctx, -1)) {
			slot = (void **) duk_get_buffer(ctx, -1, &sz);
			if (slot != NULL && sz >= sizeof(void *)) {
				slot[0] = NULL;
			}
		}
		duk_pop(ctx);
		break;
	}
	case WK_MAP:
		idx = duk__wk_ptrbuf_find(ctx, -1, STR_KEYS, target);
		if (idx >= 0) {
			duk__wk_ptrbuf_remove_at(ctx, -1, STR_KEYS, (duk_uint_t) idx);
			duk__wk_tvalbuf_swap_shrink_at(ctx, (duk_hthread *) ctx, -1,
			                                STR_VALUES, (duk_uint_t) idx);
		}
		break;
	case WK_SET:
		idx = duk__wk_ptrbuf_find(ctx, -1, STR_KEYS, target);
		if (idx >= 0) {
			duk__wk_ptrbuf_remove_at(ctx, -1, STR_KEYS, (duk_uint_t) idx);
		}
		break;
	case WK_FREG: {
		idx = duk__wk_ptrbuf_find(ctx, -1, STR_TARGETS, target);
		if (idx >= 0) {
			duk_rp_wk_table *t;
			duk_hobject *cb;
			duk_rp_wk_pending *p;
			duk_tval *tv_held;
			duk_bool_t was_empty;

			duk_get_prop_string(ctx, -1, STR_CB);
			cb = duk_get_hobject(ctx, -1);
			duk_pop(ctx);

			duk_get_prop_string(ctx, -1, STR_HELD);
			duk_get_prop_index(ctx, -1, (duk_uarridx_t) idx);
			tv_held = duk_get_tval(ctx, -1);

			t = (duk_rp_wk_table *) heap->weak_back_table;
			if (t != NULL && cb != NULL && tv_held != NULL) {
				p = (duk_rp_wk_pending *) heap->alloc_func(
				    heap->heap_udata, sizeof(*p));
				if (p != NULL) {
					p->cb = cb;
					DUK_HOBJECT_INCREF((duk_hthread *) ctx, cb);
					DUK_TVAL_SET_TVAL(&p->held, tv_held);
					DUK_TVAL_INCREF((duk_hthread *) ctx, &p->held);
					p->next = NULL;
					was_empty = (t->pending_head == NULL);
					if (t->pending_tail == NULL) {
						t->pending_head = p;
					} else {
						t->pending_tail->next = p;
					}
					t->pending_tail = p;
					/* Notify embedder on the empty -> non-empty
					 * transition only.  libevent's event_active is
					 * idempotent but cheap; we keep it edge-triggered
					 * so a single event_active per drain cycle. */
					if (was_empty && t->pending_notifier != NULL) {
						t->pending_notifier(t->pending_notifier_udata);
					}
				}
			}
			duk_pop_2(ctx);  /* held value, held array */

			duk__wk_ptrbuf_remove_at(ctx, -1, STR_TARGETS,
			                         (duk_uint_t) idx);
			duk__wk_array_swap_shrink_at(ctx, -1, STR_HELD, (duk_uint_t) idx);
			duk__wk_array_swap_shrink_at(ctx, -1, STR_TOKENS, (duk_uint_t) idx);
		}
		break;
	}
	default:
		break;
	}
	duk_pop(ctx);  /* holder */
	DUK_HEAPHDR_PREDEC_REFCOUNT((duk_heaphdr *) holder);
}

/* Ephemeron mark pass: for each REACHABLE WeakMap, for each entry whose
 * KEY is reachable and whose VALUE is heap-allocated + not-yet-reachable,
 * mark the value via the caller-supplied mark function.  Returns 1 if any
 * new marks happened (caller should iterate until 0 = fixpoint reached).
 *
 * Caller passes mark_tval_fn which is file-local to markandsweep.c.
 * Iterates heap_allocated directly; uses PREINC/PREDEC around the JS-
 * stack access of each WeakMap so we don't accidentally trip refzero. */
DUK_INTERNAL duk_bool_t duk_rp_weak_ephemeron_pass(
    duk_heap *heap,
    void (*mark_tval_fn)(duk_heap *, duk_tval *)) {
	duk_heaphdr *hdr;
	duk_context *ctx;
	duk_bool_t changed = 0;

	if (heap->heap_thread == NULL) {
		return 0;
	}
	ctx = (duk_context *) heap->heap_thread;

	for (hdr = heap->heap_allocated; hdr != NULL;
	     hdr = DUK_HEAPHDR_GET_NEXT(heap, hdr)) {
		duk_hobject *h;
		duk_uint_t kind;
		void **keys;
		duk_tval *values;
		duk_size_t keys_sz, values_sz;
		duk_uint_t count, i;

		if (DUK_HEAPHDR_GET_TYPE(hdr) != DUK_HTYPE_OBJECT) continue;
		if (!DUK_HEAPHDR_HAS_REACHABLE(hdr)) continue;
		h = (duk_hobject *) hdr;
		if (DUK_HOBJECT_GET_CLASS_NUMBER(h) != DUK_HOBJECT_CLASS_WEAK_KIND) {
			continue;
		}

		DUK_HEAPHDR_PREINC_REFCOUNT(hdr);
		duk_push_hobject(ctx, h);

		duk_get_prop_string(ctx, -1, STR_KIND);
		kind = (duk_uint_t) duk_get_uint_default(ctx, -1, 0xFFFFu);
		duk_pop(ctx);
		if (kind != WK_MAP) {
			duk_pop(ctx);
			DUK_HEAPHDR_PREDEC_REFCOUNT(hdr);
			continue;
		}

		duk_get_prop_string(ctx, -1, STR_KEYS);
		if (!duk_is_buffer(ctx, -1)) {
			duk_pop_2(ctx);
			DUK_HEAPHDR_PREDEC_REFCOUNT(hdr);
			continue;
		}
		keys = (void **) duk_get_buffer(ctx, -1, &keys_sz);
		count = (duk_uint_t) (keys_sz / sizeof(void *));

		duk_get_prop_string(ctx, -2, STR_VALUES);
		if (!duk_is_buffer(ctx, -1)) {
			duk_pop_3(ctx);
			DUK_HEAPHDR_PREDEC_REFCOUNT(hdr);
			continue;
		}
		values = (duk_tval *) duk_get_buffer(ctx, -1, &values_sz);

		for (i = 0; i < count; i++) {
			duk_hobject *k = (duk_hobject *) keys[i];
			duk_tval *tv;
			duk_heaphdr *vh;
			if (k == NULL) continue;
			if (!DUK_HEAPHDR_HAS_REACHABLE((duk_heaphdr *) k)) continue;
			if ((duk_size_t) i * sizeof(duk_tval) >= values_sz) break;
			tv = &values[i];
			if (!DUK_TVAL_IS_HEAP_ALLOCATED(tv)) continue;
			vh = DUK_TVAL_GET_HEAPHDR(tv);
			if (vh == NULL) continue;
			if (DUK_HEAPHDR_HAS_REACHABLE(vh)) continue;
			/* Key reachable, value heap-allocated and not yet marked.
			 * Mark it transitively.  mark_tval_fn will recurse through
			 * the value's own properties. */
			mark_tval_fn(heap, tv);
			changed = 1;
		}

		duk_pop_3(ctx);  /* values, keys, holder */
		DUK_HEAPHDR_PREDEC_REFCOUNT(hdr);
	}

	return changed;
}

/* Walk the pending callback queue and call the mark functions on
 * each (cb, held) pair.  Invoked from duk_heap_mark_and_sweep so the
 * queued refs survive sweep until cleanupSome / drain consumes them. */
DUK_INTERNAL void duk_rp_weak_mark_pending(
    duk_heap *heap,
    void (*mark_heaphdr_fn)(duk_heap *, duk_heaphdr *),
    void (*mark_tval_fn)(duk_heap *, duk_tval *)) {
	duk_rp_wk_table *t = (duk_rp_wk_table *) heap->weak_back_table;
	duk_rp_wk_pending *p;
	if (t == NULL) {
		return;
	}
	for (p = t->pending_head; p != NULL; p = p->next) {
		if (p->cb != NULL) {
			mark_heaphdr_fn(heap, (duk_heaphdr *) p->cb);
		}
		mark_tval_fn(heap, &p->held);
	}
}

DUK_INTERNAL void duk_rp_weak_back_target_dying(duk_heap *heap,
                                                duk_hobject *obj) {
	duk_rp_wk_table *t = (duk_rp_wk_table *) heap->weak_back_table;
	duk_rp_wk_back_bucket *b;

	if (t == NULL) {
		return;
	}

	/* Target-role cleanup: if `obj` is a target referenced by any
	 * WEAK_KIND holder, do per-role cleanup on each. */
	b = duk__wk_bucket_find(t, (duk_uintptr_t) obj);
	if (b != NULL) {
		duk_rp_wk_back_entry *e = b->first;
		while (e != NULL) {
			duk_rp_wk_back_entry *next_e = e->next;
			duk__wk_cleanup_for_target(heap, obj, e->holder, e->role);
			heap->free_func(heap->heap_udata, e);
			e = next_e;
		}
		b->first = NULL;
		duk__wk_bucket_remove(t, (duk_uintptr_t) obj);
	}

	/* Holder-role cleanup is delegated entirely to the finalizer
	 * attached in the ctor.  Doing it here too would duplicate work
	 * and complicate the refcount juggling around duk_push_hobject
	 * on a dying object. */
}

static void duk__wk_holder_unregister_all(duk_heap *heap, duk_hobject *holder) {
	duk_context *ctx = (duk_context *) heap->heap_thread;
	duk_uint_t kind;
	const char *buf_key = NULL;
	duk_uint_t count, i;
	void **arr;
	duk_size_t sz;

	if (ctx == NULL) {
		return;
	}
	duk_push_hobject(ctx, holder);
	duk_get_prop_string(ctx, -1, STR_KIND);
	kind = (duk_uint_t) duk_get_uint_default(ctx, -1, 0xFFFFu);
	duk_pop(ctx);
	switch (kind) {
	case WK_REF:  buf_key = STR_TARGET;  break;
	case WK_MAP:  buf_key = STR_KEYS;    break;
	case WK_SET:  buf_key = STR_KEYS;    break;
	case WK_FREG: buf_key = STR_TARGETS; break;
	default:
		duk_pop(ctx);
		return;
	}
	duk_get_prop_string(ctx, -1, buf_key);
	if (!duk_is_buffer(ctx, -1)) {
		duk_pop_2(ctx);
		return;
	}
	arr = (void **) duk_get_buffer(ctx, -1, &sz);
	count = (duk_uint_t) (sz / sizeof(void *));
	for (i = 0; i < count; i++) {
		if (arr[i] != NULL) {
			duk__wk_back_remove_holder(heap, (duk_hobject *) arr[i],
			                            holder, kind);
		}
	}
	duk_pop(ctx);  /* keys/targets buffer */

	/* WeakMap values buffer holds INCREF'd tvals that won't be DECREF'd
	 * by duktape's normal teardown (buffer bytes aren't walked).  Clear
	 * the buffer ourselves so we don't leak the values. */
	if (kind == WK_MAP) {
		duk__wk_tvalbuf_clear(ctx, (duk_hthread *) ctx, -1, STR_VALUES);
	}
	duk_pop(ctx);  /* holder */
}

/* ------------------------------------------------------------------ *
 *  GC hooks: post-mark cleanup                                        *
 * ------------------------------------------------------------------ */

/* Run after marking is complete and before sweep.  Walks the heap and
 * resolves weak relationships: any target whose REACHABLE flag is
 * clear is treated as dying and its back-list is drained. */
DUK_INTERNAL void duk_rp_weak_postmark_cleanup(duk_heap *heap) {
	duk_rp_wk_table *t = (duk_rp_wk_table *) heap->weak_back_table;
	duk_uint_t bi;

	if (t == NULL) {
		return;
	}
	/* Iterate buckets; collect dead-target keys first to avoid
	 * mutating the table while iterating. */
	for (bi = 0; bi < t->bucket_count; bi++) {
		duk_rp_wk_back_bucket *b = t->buckets[bi];
		duk_rp_wk_back_bucket *next_b;
		while (b != NULL) {
			next_b = b->next;
			{
				duk_hobject *target = (duk_hobject *) (void *) b->target_key;
				duk_heaphdr *hdr = (duk_heaphdr *) target;
				if (!DUK_HEAPHDR_HAS_REACHABLE(hdr)) {
					/* Target is unreachable; cleanup all holders and
					 * remove the bucket entry. */
					duk_rp_wk_back_entry *e = b->first;
					while (e != NULL) {
						duk_rp_wk_back_entry *next_e = e->next;
						if (DUK_HEAPHDR_HAS_REACHABLE((duk_heaphdr *) e->holder)) {
							duk__wk_cleanup_for_target(heap, target,
							                            e->holder, e->role);
						}
						heap->free_func(heap->heap_udata, e);
						e = next_e;
					}
					b->first = NULL;
					duk__wk_bucket_remove(t, b->target_key);
				}
			}
			b = next_b;
		}
	}
}

/* ------------------------------------------------------------------ *
 *  Heap free: release back-table and pending queue                    *
 * ------------------------------------------------------------------ */

DUK_INTERNAL void duk_rp_weak_back_table_free(duk_heap *heap) {
	duk_rp_wk_table *t = (duk_rp_wk_table *) heap->weak_back_table;
	duk_uint_t bi;
	if (t == NULL) {
		return;
	}
	for (bi = 0; bi < t->bucket_count; bi++) {
		duk_rp_wk_back_bucket *b = t->buckets[bi];
		while (b != NULL) {
			duk_rp_wk_back_bucket *next_b = b->next;
			duk_rp_wk_back_entry *e = b->first;
			while (e != NULL) {
				duk_rp_wk_back_entry *next_e = e->next;
				heap->free_func(heap->heap_udata, e);
				e = next_e;
			}
			heap->free_func(heap->heap_udata, b);
			b = next_b;
		}
	}
	heap->free_func(heap->heap_udata, t->buckets);
	while (t->pending_head != NULL) {
		duk_rp_wk_pending *p = t->pending_head;
		t->pending_head = p->next;
		heap->free_func(heap->heap_udata, p);
	}
	heap->free_func(heap->heap_udata, t);
	heap->weak_back_table = NULL;
}

/* ------------------------------------------------------------------ *
 *  @@toStringTag accessors                                            *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__wkref_tag(duk_context *ctx) {
	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_REF)) {
		duk_push_undefined(ctx);
	} else {
		duk_push_string(ctx, "WeakRef");
	}
	return 1;
}
static duk_ret_t duk__wkmap_tag(duk_context *ctx) {
	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_MAP)) {
		duk_push_undefined(ctx);
	} else {
		duk_push_string(ctx, "WeakMap");
	}
	return 1;
}
static duk_ret_t duk__wkset_tag(duk_context *ctx) {
	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_SET)) {
		duk_push_undefined(ctx);
	} else {
		duk_push_string(ctx, "WeakSet");
	}
	return 1;
}
static duk_ret_t duk__finreg_tag(duk_context *ctx) {
	duk_push_this(ctx);
	if (!duk__wk_is_kind(ctx, -1, WK_FREG)) {
		duk_push_undefined(ctx);
	} else {
		duk_push_string(ctx, "FinalizationRegistry");
	}
	return 1;
}

static void duk__wk_install_tag_accessor(duk_context *ctx, duk_idx_t proto_idx,
                                          duk_c_function getter) {
	proto_idx = duk_normalize_index(ctx, proto_idx);
	duk_get_global_string(ctx, "Symbol");
	duk_get_prop_string(ctx, -1, "toStringTag");
	duk_remove(ctx, -2);  /* Symbol global */
	duk_push_c_function(ctx, getter, 0);
	duk_def_prop(ctx, proto_idx,
	             DUK_DEFPROP_HAVE_GETTER |
	             DUK_DEFPROP_HAVE_ENUMERABLE |
	             DUK_DEFPROP_HAVE_CONFIGURABLE |
	             DUK_DEFPROP_CONFIGURABLE);
}

/* ------------------------------------------------------------------ *
 *  Installer                                                          *
 * ------------------------------------------------------------------ */

static void duk__wk_install_one(duk_context *ctx, const char *name,
                                duk_c_function ctor, duk_idx_t nargs,
                                const duk_function_list_entry *methods,
                                duk_c_function tag_getter) {
	duk_push_c_function(ctx, ctor, nargs);

	/* Set ctor.name (non-writable, configurable per spec). */
	duk_push_string(ctx, "name");
	duk_push_string(ctx, name);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE |
	                       DUK_DEFPROP_HAVE_CONFIGURABLE |
	                       DUK_DEFPROP_CONFIGURABLE |
	                       DUK_DEFPROP_FORCE);

	/* Build the prototype and attach methods. */
	duk_push_object(ctx);
	if (methods != NULL) {
		duk_put_function_list(ctx, -1, methods);
	}
	if (tag_getter != NULL) {
		duk__wk_install_tag_accessor(ctx, -1, tag_getter);
	}
	/* prototype.constructor = ctor */
	duk_dup(ctx, -2);
	duk_put_prop_string(ctx, -2, "constructor");
	/* ctor.prototype = proto */
	duk_put_prop_string(ctx, -2, "prototype");

	duk_put_global_string(ctx, name);
}

static const duk_function_list_entry duk__wkref_methods[] = {
	{ "deref", duk__wkref_deref, 0 },
	{ NULL, NULL, 0 }
};

static const duk_function_list_entry duk__wkmap_methods[] = {
	{ "get",    duk__wkmap_get,    1 },
	{ "set",    duk__wkmap_set,    2 },
	{ "has",    duk__wkmap_has,    1 },
	{ "delete", duk__wkmap_delete, 1 },
	{ NULL, NULL, 0 }
};

static const duk_function_list_entry duk__wkset_methods[] = {
	{ "add",    duk__wkset_add,    1 },
	{ "has",    duk__wkset_has,    1 },
	{ "delete", duk__wkset_delete, 1 },
	{ NULL, NULL, 0 }
};

static const duk_function_list_entry duk__finreg_methods[] = {
	{ "register",    duk__finreg_register,     3 },
	{ "unregister",  duk__finreg_unregister,   1 },
	{ "cleanupSome", duk__finreg_cleanup_some, 1 },
	{ NULL, NULL, 0 }
};

DUK_INTERNAL void duk_rp_install_weak_refs(duk_context *ctx) {
	duk__wk_install_one(ctx, "WeakRef",
	                    duk__wkref_ctor, 1,
	                    duk__wkref_methods, duk__wkref_tag);
	duk__wk_install_one(ctx, "WeakMap",
	                    duk__wkmap_ctor, 1,
	                    duk__wkmap_methods, duk__wkmap_tag);
	duk__wk_install_one(ctx, "WeakSet",
	                    duk__wkset_ctor, 1,
	                    duk__wkset_methods, duk__wkset_tag);
	duk__wk_install_one(ctx, "FinalizationRegistry",
	                    duk__finreg_ctor, 1,
	                    duk__finreg_methods, duk__finreg_tag);
}

#endif  /* DUK_RP_USE_WEAK_REFS */
