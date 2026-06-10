/*
 *  duk_rp_promise_native.c
 *
 *  Native Promise implementation for duktape 2.7.0 (rampart contribution).
 *  Gated by DUK_RP_USE_PROMISE_NATIVE.  When defined, replaces the JS
 *  polyfill from duk_rp_promise.c -- the polyfill installer becomes a
 *  no-op (see extensions_init switching).
 *
 *  Implements ES2022 Promise: constructor, then, catch, finally,
 *  resolve, reject, all.  Skips race / allSettled / any in v1 (can be
 *  added later via the same pattern).
 *
 *  Design
 *  ------
 *  Each Promise is a regular hobject with class OBJECT (no class slot
 *  needed -- brand checks go through a hidden internal-key prop).
 *  Internal slots stored as hidden properties:
 *    \xff_p_state   number   0=pending, 1=fulfilled, 2=rejected
 *    \xff_p_result  any      fulfillment value or rejection reason
 *    \xff_p_fq      Array    pending fulfill reactions (only while pending)
 *    \xff_p_rq      Array    pending reject reactions  (only while pending)
 *    \xff_p_handled bool     set true when a handler is attached via .then
 *    \xff_p_brand   true     identifies the object as a real native Promise
 *
 *  Reactions are stored as plain objects with hidden props:
 *    \xff_r_type     number   0=fulfill, 1=reject
 *    \xff_r_handler  fn|null  the user callback (null = pass-through)
 *    \xff_r_resolve  function resolve capability of the result promise
 *    \xff_r_reject   function reject capability of the result promise
 *
 *  Microtask queue is a heap-level linked list of duk_rp_microtask
 *  structs.  Two job types: REACTION and THENABLE.  GC mark hook marks
 *  all referenced hobjects + tvals so they survive sweep.
 *
 *  Drainer + notifier mirror the FinReg pattern: drain runs the queue
 *  to empty; notifier fires on the empty -> non-empty transition.
 *  Rampart wires the notifier to libevent's event_active so microtasks
 *  drain at the end of each event-loop iteration.  Vanilla duktape
 *  embedders call duk_rp_microtask_drain(ctx) explicitly (e.g. between
 *  top-level evaluations).
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_PROMISE_NATIVE)

/* ------------------------------------------------------------------ *
 *  Internal key strings and small enums                               *
 * ------------------------------------------------------------------ */

static const char STR_P_STATE[]   = "\xff" "p_state";
static const char STR_P_RESULT[]  = "\xff" "p_result";
static const char STR_P_FQ[]      = "\xff" "p_fq";
static const char STR_P_RQ[]      = "\xff" "p_rq";
static const char STR_P_HANDLED[] = "\xff" "p_handled";
static const char STR_P_BRAND[]   = "\xff" "p_brand";

static const char STR_R_TYPE[]    = "\xff" "r_type";
static const char STR_R_HANDLER[] = "\xff" "r_handler";
static const char STR_R_RESOLVE[] = "\xff" "r_resolve";
static const char STR_R_REJECT[]  = "\xff" "r_reject";

static const char STR_CAP_TARGET[] = "\xff" "cap_target";
static const char STR_CAP_DONE[]   = "\xff" "cap_done";

#define PSTATE_PENDING   0
#define PSTATE_FULFILLED 1
#define PSTATE_REJECTED  2

#define RTYPE_FULFILL 0
#define RTYPE_REJECT  1

#define MT_REACTION 0
#define MT_THENABLE 1

/* ------------------------------------------------------------------ *
 *  Microtask queue                                                    *
 * ------------------------------------------------------------------ */

typedef struct duk_rp_microtask duk_rp_microtask;
struct duk_rp_microtask {
	duk_uint_t type;            /* MT_REACTION or MT_THENABLE */
	/* Common to both */
	duk_tval value;             /* REACTION: value to pass to handler.
	                             * THENABLE: unused. */
	duk_hobject *a;             /* REACTION: handler (may be NULL).
	                             * THENABLE: promise being resolved. */
	duk_hobject *b;             /* REACTION: resolve capability.
	                             * THENABLE: thenable. */
	duk_hobject *c;             /* REACTION: reject capability.
	                             * THENABLE: then function. */
	duk_uint_t  reaction_type;  /* REACTION only: RTYPE_FULFILL or _REJECT */
	duk_rp_microtask *next;
};

typedef struct duk_rp_mt_queue duk_rp_mt_queue;
struct duk_rp_mt_queue {
	duk_rp_microtask *head;
	duk_rp_microtask *tail;
	void (*notifier)(void *udata);
	void *notifier_udata;
};

/* ------------------------------------------------------------------ *
 *  Queue access helpers                                               *
 * ------------------------------------------------------------------ */

static duk_rp_mt_queue *duk__pmt_queue_ensure(duk_heap *heap) {
	duk_rp_mt_queue *q = (duk_rp_mt_queue *) heap->microtask_queue;
	if (q != NULL) {
		return q;
	}
	q = (duk_rp_mt_queue *) heap->alloc_func(heap->heap_udata, sizeof(*q));
	if (q == NULL) {
		return NULL;
	}
	q->head = NULL;
	q->tail = NULL;
	q->notifier = NULL;
	q->notifier_udata = NULL;
	heap->microtask_queue = (void *) q;
	return q;
}

static void duk__pmt_enqueue(duk_rp_microtask *m, duk_heap *heap) {
	duk_rp_mt_queue *q = duk__pmt_queue_ensure(heap);
	duk_bool_t was_empty;
	if (q == NULL) {
		heap->free_func(heap->heap_udata, m);
		return;
	}
	was_empty = (q->head == NULL);
	m->next = NULL;
	if (q->tail == NULL) {
		q->head = m;
	} else {
		q->tail->next = m;
	}
	q->tail = m;
	if (was_empty && q->notifier != NULL) {
		q->notifier(q->notifier_udata);
	}
}

static void duk__pmt_release(duk_hthread *thr, duk_rp_microtask *m) {
	/* DECREF all owned refs.  Use NORZ so we don't trigger refzero
	 * cascades mid-drain.  This is called after the microtask body
	 * has run and we're freeing the node. */
	DUK_TVAL_DECREF_NORZ(thr, &m->value);
	if (m->a != NULL) DUK_HOBJECT_DECREF_NORZ(thr, m->a);
	if (m->b != NULL) DUK_HOBJECT_DECREF_NORZ(thr, m->b);
	if (m->c != NULL) DUK_HOBJECT_DECREF_NORZ(thr, m->c);
	thr->heap->free_func(thr->heap->heap_udata, m);
}

/* ------------------------------------------------------------------ *
 *  Forward declarations                                                *
 * ------------------------------------------------------------------ */

static duk_bool_t duk__pis_promise(duk_context *ctx, duk_idx_t idx);
static void duk__presolve_promise(duk_context *ctx, duk_idx_t promise_idx,
                                   duk_idx_t value_idx);
static void duk__preject_promise(duk_context *ctx, duk_idx_t promise_idx,
                                  duk_idx_t reason_idx);
static void duk__pfulfill_promise(duk_context *ctx, duk_idx_t promise_idx,
                                   duk_idx_t value_idx);
static void duk__pmark_handled(duk_context *ctx, duk_idx_t promise_idx);

static duk_ret_t duk__pmt_reaction_run(duk_context *ctx, void *udata);
static duk_ret_t duk__pmt_thenable_run(duk_context *ctx, void *udata);

/* ------------------------------------------------------------------ *
 *  Promise brand check                                                *
 * ------------------------------------------------------------------ */

static duk_bool_t duk__pis_promise(duk_context *ctx, duk_idx_t idx) {
	duk_bool_t r;
	if (!duk_is_object(ctx, idx)) {
		return 0;
	}
	duk_get_prop_string(ctx, idx, STR_P_BRAND);
	r = duk_to_boolean(ctx, -1);
	duk_pop(ctx);
	return r;
}

/* ------------------------------------------------------------------ *
 *  Reaction enqueue                                                   *
 * ------------------------------------------------------------------ */

/* Enqueue a reaction microtask.  Stack args:
 *   value_idx -> the value to pass through (fulfillment value or
 *                rejection reason)
 *   handler   -> hobject (may be NULL for pass-through)
 *   resolve   -> hobject (resolve capability of the result promise)
 *   reject    -> hobject (reject capability of the result promise)
 *   rtype     -> RTYPE_FULFILL or RTYPE_REJECT (for pass-through behavior)
 */
static void duk__pmt_enqueue_reaction(duk_context *ctx,
                                       duk_idx_t value_idx,
                                       duk_hobject *handler,
                                       duk_hobject *resolve_cap,
                                       duk_hobject *reject_cap,
                                       duk_uint_t rtype) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_rp_microtask *m;
	duk_tval *tv;
	m = (duk_rp_microtask *) thr->heap->alloc_func(thr->heap->heap_udata, sizeof(*m));
	if (m == NULL) {
		(void) duk_range_error(ctx, "Promise: out of memory");
		return;
	}
	m->type = MT_REACTION;
	m->reaction_type = rtype;
	tv = duk_get_tval(ctx, value_idx);
	DUK_TVAL_SET_TVAL(&m->value, tv);
	DUK_TVAL_INCREF(thr, &m->value);
	m->a = handler;
	if (handler != NULL) DUK_HOBJECT_INCREF(thr, handler);
	m->b = resolve_cap;
	if (resolve_cap != NULL) DUK_HOBJECT_INCREF(thr, resolve_cap);
	m->c = reject_cap;
	if (reject_cap != NULL) DUK_HOBJECT_INCREF(thr, reject_cap);
	duk__pmt_enqueue(m, thr->heap);
}

/* Enqueue a thenable adoption microtask.  Stack args:
 *   promise  -> the promise being resolved
 *   thenable -> the thenable to adopt
 *   then_fn  -> thenable.then function
 */
static void duk__pmt_enqueue_thenable(duk_context *ctx,
                                       duk_hobject *promise,
                                       duk_hobject *thenable,
                                       duk_hobject *then_fn) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_rp_microtask *m;
	m = (duk_rp_microtask *) thr->heap->alloc_func(thr->heap->heap_udata, sizeof(*m));
	if (m == NULL) {
		(void) duk_range_error(ctx, "Promise: out of memory");
		return;
	}
	m->type = MT_THENABLE;
	DUK_TVAL_SET_UNDEFINED(&m->value);
	m->a = promise;  DUK_HOBJECT_INCREF(thr, promise);
	m->b = thenable; DUK_HOBJECT_INCREF(thr, thenable);
	m->c = then_fn;  DUK_HOBJECT_INCREF(thr, then_fn);
	duk__pmt_enqueue(m, thr->heap);
}

/* ------------------------------------------------------------------ *
 *  Promise state helpers                                              *
 * ------------------------------------------------------------------ */

/* Read state, push result + state. */
static duk_uint_t duk__pstate_get(duk_context *ctx, duk_idx_t idx) {
	duk_uint_t s;
	duk_get_prop_string(ctx, idx, STR_P_STATE);
	s = (duk_uint_t) duk_to_uint(ctx, -1);
	duk_pop(ctx);
	return s;
}

static void duk__pstate_set(duk_context *ctx, duk_idx_t idx, duk_uint_t state) {
	idx = duk_normalize_index(ctx, idx);
	duk_push_uint(ctx, state);
	duk_put_prop_string(ctx, idx, STR_P_STATE);
}

static void duk__pmark_handled(duk_context *ctx, duk_idx_t idx) {
	idx = duk_normalize_index(ctx, idx);
	duk_push_true(ctx);
	duk_put_prop_string(ctx, idx, STR_P_HANDLED);
}

/* Pop all reactions from the named queue and schedule them as
 * microtasks.  Pops the queue array off the promise too. */
static void duk__ptrigger_reactions(duk_context *ctx, duk_idx_t promise_idx,
                                     const char *queue_key, duk_uint_t rtype,
                                     duk_idx_t value_idx) {
	duk_uint_t len, i;
	promise_idx = duk_normalize_index(ctx, promise_idx);
	duk_get_prop_string(ctx, promise_idx, queue_key);
	if (!duk_is_array(ctx, -1)) {
		duk_pop(ctx);
		return;
	}
	len = (duk_uint_t) duk_get_length(ctx, -1);
	for (i = 0; i < len; i++) {
		duk_hobject *handler;
		duk_hobject *resolve_cap;
		duk_hobject *reject_cap;
		duk_get_prop_index(ctx, -1, i);  /* reaction */
		duk_get_prop_string(ctx, -1, STR_R_HANDLER);
		handler = duk_is_function(ctx, -1) ? duk_get_hobject(ctx, -1) : NULL;
		duk_pop(ctx);
		duk_get_prop_string(ctx, -1, STR_R_RESOLVE);
		resolve_cap = duk_get_hobject(ctx, -1);
		duk_pop(ctx);
		duk_get_prop_string(ctx, -1, STR_R_REJECT);
		reject_cap = duk_get_hobject(ctx, -1);
		duk_pop(ctx);
		duk_pop(ctx);  /* reaction */
		duk__pmt_enqueue_reaction(ctx, value_idx, handler,
		                           resolve_cap, reject_cap, rtype);
	}
	duk_pop(ctx);  /* queue */
}

static void duk__pfulfill_promise(duk_context *ctx, duk_idx_t promise_idx,
                                   duk_idx_t value_idx) {
	promise_idx = duk_normalize_index(ctx, promise_idx);
	value_idx = duk_normalize_index(ctx, value_idx);
	if (duk__pstate_get(ctx, promise_idx) != PSTATE_PENDING) {
		return;
	}
	duk_dup(ctx, value_idx);
	duk_put_prop_string(ctx, promise_idx, STR_P_RESULT);
	duk__pstate_set(ctx, promise_idx, PSTATE_FULFILLED);
	duk__ptrigger_reactions(ctx, promise_idx, STR_P_FQ, RTYPE_FULFILL, value_idx);
	/* Clear both queues -- they'll never be needed again. */
	duk_push_undefined(ctx);
	duk_put_prop_string(ctx, promise_idx, STR_P_FQ);
	duk_push_undefined(ctx);
	duk_put_prop_string(ctx, promise_idx, STR_P_RQ);
}

static void duk__preject_promise(duk_context *ctx, duk_idx_t promise_idx,
                                  duk_idx_t reason_idx) {
	promise_idx = duk_normalize_index(ctx, promise_idx);
	reason_idx = duk_normalize_index(ctx, reason_idx);
	if (duk__pstate_get(ctx, promise_idx) != PSTATE_PENDING) {
		return;
	}
	duk_dup(ctx, reason_idx);
	duk_put_prop_string(ctx, promise_idx, STR_P_RESULT);
	duk__pstate_set(ctx, promise_idx, PSTATE_REJECTED);
	duk__ptrigger_reactions(ctx, promise_idx, STR_P_RQ, RTYPE_REJECT, reason_idx);
	duk_push_undefined(ctx);
	duk_put_prop_string(ctx, promise_idx, STR_P_FQ);
	duk_push_undefined(ctx);
	duk_put_prop_string(ctx, promise_idx, STR_P_RQ);
}

/* The Promise Resolution Procedure (spec 25.6.1.7).
 * promise_idx is target promise; resolution_idx is the value to resolve with. */
static void duk__presolve_promise(duk_context *ctx, duk_idx_t promise_idx,
                                   duk_idx_t resolution_idx) {
	duk_hthread *thr = (duk_hthread *) ctx;
	promise_idx = duk_normalize_index(ctx, promise_idx);
	resolution_idx = duk_normalize_index(ctx, resolution_idx);

	if (duk__pstate_get(ctx, promise_idx) != PSTATE_PENDING) {
		return;
	}

	/* If resolution is the same promise -> TypeError */
	if (duk_strict_equals(ctx, promise_idx, resolution_idx)) {
		duk_push_error_object(ctx, DUK_ERR_TYPE_ERROR,
		    "A promise cannot be resolved with itself.");
		duk__preject_promise(ctx, promise_idx, duk_get_top(ctx) - 1);
		duk_pop(ctx);
		return;
	}

	/* Non-object: fulfill directly */
	if (!duk_is_object(ctx, resolution_idx)) {
		duk__pfulfill_promise(ctx, promise_idx, resolution_idx);
		return;
	}

	/* If resolution is our own Promise instance, adopt its state */
	if (duk__pis_promise(ctx, resolution_idx)) {
		duk_uint_t s = duk__pstate_get(ctx, resolution_idx);
		if (s == PSTATE_FULFILLED) {
			duk_get_prop_string(ctx, resolution_idx, STR_P_RESULT);
			duk__pfulfill_promise(ctx, promise_idx, duk_get_top(ctx) - 1);
			duk_pop(ctx);
			return;
		}
		if (s == PSTATE_REJECTED) {
			duk_get_prop_string(ctx, resolution_idx, STR_P_RESULT);
			duk__preject_promise(ctx, promise_idx, duk_get_top(ctx) - 1);
			duk_pop(ctx);
			return;
		}
		/* Pending: need to wait.  Chain via .then(internal-resolve, internal-reject).
		 * Falls through to the thenable adoption path below. */
	}

	/* Get .then to check if thenable */
	if (duk_get_prop_string(ctx, resolution_idx, "then")) {
		/* Non-undefined .then; if function, adopt as thenable */
		if (duk_is_function(ctx, -1)) {
			duk_hobject *then_fn = duk_get_hobject(ctx, -1);
			duk_hobject *thenable = duk_get_hobject(ctx, resolution_idx);
			duk_hobject *promise = duk_get_hobject(ctx, promise_idx);
			/* CRITICAL: enqueue BEFORE pop -- enqueue_thenable INCREFs
			 * then_fn, but if we pop first then_fn's refcount may hit
			 * zero and the storage gets freed before INCREF runs.  Same
			 * stack-keep-alive principle that bit us in the FinReg
			 * holder push pattern. */
			duk__pmt_enqueue_thenable(ctx, promise, thenable, then_fn);
			duk_pop(ctx);  /* then */
			(void) thr;
			return;
		}
		duk_pop(ctx);  /* not a function */
	} else {
		duk_pop(ctx);  /* undefined .then */
	}

	/* Not a thenable -- fulfill with the value */
	duk__pfulfill_promise(ctx, promise_idx, resolution_idx);
}

/* ------------------------------------------------------------------ *
 *  Capability functions (resolve / reject created per-promise)        *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__pcap_resolve(duk_context *ctx) {
	duk_hobject *target;
	duk_push_current_function(ctx);
	/* Idempotent: skip if already called */
	duk_get_prop_string(ctx, -1, STR_CAP_DONE);
	if (duk_to_boolean(ctx, -1)) { return 0; }
	duk_pop(ctx);
	duk_push_true(ctx);
	duk_put_prop_string(ctx, -2, STR_CAP_DONE);
	/* Load target promise */
	duk_get_prop_string(ctx, -1, STR_CAP_TARGET);
	target = duk_get_hobject(ctx, -1);
	if (target == NULL) {
		duk_pop_2(ctx);
		return 0;
	}
	/* Stack: [arg, ..., this_fn, target] */
	if (duk_get_top(ctx) < 2) {
		duk_push_undefined(ctx);  /* arg 0 default */
		duk_insert(ctx, 0);
	}
	duk__presolve_promise(ctx, -1, 0);
	duk_pop_2(ctx);
	return 0;
}

static duk_ret_t duk__pcap_reject(duk_context *ctx) {
	duk_hobject *target;
	duk_push_current_function(ctx);
	duk_get_prop_string(ctx, -1, STR_CAP_DONE);
	if (duk_to_boolean(ctx, -1)) { return 0; }
	duk_pop(ctx);
	duk_push_true(ctx);
	duk_put_prop_string(ctx, -2, STR_CAP_DONE);
	duk_get_prop_string(ctx, -1, STR_CAP_TARGET);
	target = duk_get_hobject(ctx, -1);
	if (target == NULL) {
		duk_pop_2(ctx);
		return 0;
	}
	if (duk_get_top(ctx) < 2) {
		duk_push_undefined(ctx);
		duk_insert(ctx, 0);
	}
	duk__preject_promise(ctx, -1, 0);
	duk_pop_2(ctx);
	return 0;
}

/* Create and push a fresh resolve/reject pair bound to the promise
 * currently at promise_idx.  After call, two functions are pushed:
 *   [..., promise, ..., resolve, reject]
 * The caller is responsible for stack management. */
static void duk__pcap_pair_push(duk_context *ctx, duk_idx_t promise_idx) {
	promise_idx = duk_normalize_index(ctx, promise_idx);

	duk_push_c_function(ctx, duk__pcap_resolve, 1);
	duk_dup(ctx, promise_idx);
	duk_put_prop_string(ctx, -2, STR_CAP_TARGET);
	duk_push_false(ctx);
	duk_put_prop_string(ctx, -2, STR_CAP_DONE);

	duk_push_c_function(ctx, duk__pcap_reject, 1);
	duk_dup(ctx, promise_idx);
	duk_put_prop_string(ctx, -2, STR_CAP_TARGET);
	duk_push_false(ctx);
	duk_put_prop_string(ctx, -2, STR_CAP_DONE);
}

/* ------------------------------------------------------------------ *
 *  Promise constructor                                                *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__promise_ctor(duk_context *ctx) {
	duk_idx_t this_idx;

	if (!duk_is_constructor_call(ctx)) {
		return duk_type_error(ctx,
		    "Promise constructor requires 'new'");
	}
	if (!duk_is_function(ctx, 0)) {
		return duk_type_error(ctx,
		    "Promise resolver is not a function");
	}

	duk_push_this(ctx);
	this_idx = duk_get_top(ctx) - 1;

	/* Initialize internal slots */
	duk_push_uint(ctx, PSTATE_PENDING);
	duk_put_prop_string(ctx, this_idx, STR_P_STATE);
	duk_push_undefined(ctx);
	duk_put_prop_string(ctx, this_idx, STR_P_RESULT);
	duk_push_array(ctx);
	duk_put_prop_string(ctx, this_idx, STR_P_FQ);
	duk_push_array(ctx);
	duk_put_prop_string(ctx, this_idx, STR_P_RQ);
	duk_push_false(ctx);
	duk_put_prop_string(ctx, this_idx, STR_P_HANDLED);
	duk_push_true(ctx);
	duk_put_prop_string(ctx, this_idx, STR_P_BRAND);

	/* Build resolve/reject pair bound to this */
	duk__pcap_pair_push(ctx, this_idx);
	/* Stack: [executor, ..., this, resolve, reject] */

	/* Invoke executor(resolve, reject).  If it throws synchronously,
	 * we reject the promise with the error. */
	duk_dup(ctx, 0);           /* executor */
	duk_insert(ctx, -3);        /* [..., executor, resolve, reject] */
	if (duk_pcall(ctx, 2) != 0) {
		/* Error on top */
		duk__preject_promise(ctx, this_idx, duk_get_top(ctx) - 1);
	}
	duk_set_top(ctx, this_idx + 1);  /* keep this */
	return 0;  /* `this` returned implicitly */
}

/* ------------------------------------------------------------------ *
 *  Promise.prototype.then                                             *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__promise_then(duk_context *ctx) {
	duk_idx_t this_idx, new_idx, reaction_idx;
	duk_hobject *handler_fulfill = NULL;
	duk_hobject *handler_reject = NULL;
	duk_hobject *resolve_cap, *reject_cap;
	duk_uint_t state;

	duk_push_this(ctx);
	this_idx = duk_get_top(ctx) - 1;

	/* If `this` isn't a native promise, adopt it via Promise.resolve. */
	if (!duk__pis_promise(ctx, this_idx)) {
		duk_get_global_string(ctx, "Promise");
		duk_get_prop_string(ctx, -1, "resolve");
		duk_insert(ctx, -2);  /* [..., resolve, Promise] */
		duk_dup(ctx, this_idx);
		duk_call_method(ctx, 1);  /* Promise.resolve(this) */
		/* Now stack top is an adopted promise.  Call .then on it. */
		duk_get_prop_string(ctx, -1, "then");
		duk_insert(ctx, -2);
		if (duk_get_top(ctx) >= 1) duk_dup(ctx, 0); else duk_push_undefined(ctx);
		if (duk_get_top(ctx) >= 2) duk_dup(ctx, 1); else duk_push_undefined(ctx);
		duk_call_method(ctx, 2);
		return 1;
	}

	if (duk_is_function(ctx, 0)) handler_fulfill = duk_get_hobject(ctx, 0);
	if (duk_is_function(ctx, 1)) handler_reject  = duk_get_hobject(ctx, 1);

	/* Mark this promise as handled regardless -- attaching any .then
	 * counts as handling per spec. */
	duk__pmark_handled(ctx, this_idx);

	/* Build the result promise via the constructor (so internal slots
	 * are correctly initialized).  We call Promise(noop) to get a fresh
	 * pending promise. */
	duk_get_global_string(ctx, "Promise");
	{
		/* Push noop executor that captures resolve/reject via local
		 * closure.  Simplest: we use a small JS-eval hack to construct
		 * directly with pcap_pair_push instead. */
		duk_push_object(ctx);
		new_idx = duk_get_top(ctx) - 1;

		/* Set the prototype chain to Promise.prototype */
		duk_dup(ctx, -2);  /* Promise */
		duk_get_prop_string(ctx, -1, "prototype");
		duk_remove(ctx, -2);  /* remove the Promise we re-dup'd */
		duk_set_prototype(ctx, new_idx);

		duk_push_uint(ctx, PSTATE_PENDING);
		duk_put_prop_string(ctx, new_idx, STR_P_STATE);
		duk_push_undefined(ctx);
		duk_put_prop_string(ctx, new_idx, STR_P_RESULT);
		duk_push_array(ctx);
		duk_put_prop_string(ctx, new_idx, STR_P_FQ);
		duk_push_array(ctx);
		duk_put_prop_string(ctx, new_idx, STR_P_RQ);
		duk_push_false(ctx);
		duk_put_prop_string(ctx, new_idx, STR_P_HANDLED);
		duk_push_true(ctx);
		duk_put_prop_string(ctx, new_idx, STR_P_BRAND);
	}
	duk_remove(ctx, -2);  /* remove Promise */
	new_idx = duk_get_top(ctx) - 1;

	/* Build capability pair for new promise */
	duk__pcap_pair_push(ctx, new_idx);
	reject_cap = duk_get_hobject(ctx, -1);
	resolve_cap = duk_get_hobject(ctx, -2);

	state = duk__pstate_get(ctx, this_idx);
	if (state == PSTATE_PENDING) {
		/* Append reactions to queues */
		duk_uint_t i;
		const char *queues[2] = { STR_P_FQ, STR_P_RQ };
		duk_hobject *handlers[2] = { handler_fulfill, handler_reject };
		duk_uint_t types[2] = { RTYPE_FULFILL, RTYPE_REJECT };
		for (i = 0; i < 2; i++) {
			duk_get_prop_string(ctx, this_idx, queues[i]);
			if (!duk_is_array(ctx, -1)) {
				duk_pop(ctx);
				continue;
			}
			duk_push_object(ctx);
			reaction_idx = duk_get_top(ctx) - 1;
			duk_push_uint(ctx, types[i]);
			duk_put_prop_string(ctx, reaction_idx, STR_R_TYPE);
			if (handlers[i] != NULL) duk_push_hobject(ctx, handlers[i]);
			else                     duk_push_null(ctx);
			duk_put_prop_string(ctx, reaction_idx, STR_R_HANDLER);
			duk_push_hobject(ctx, resolve_cap);
			duk_put_prop_string(ctx, reaction_idx, STR_R_RESOLVE);
			duk_push_hobject(ctx, reject_cap);
			duk_put_prop_string(ctx, reaction_idx, STR_R_REJECT);
			/* append: queue[queue.length] = reaction */
			duk_uint_t len = (duk_uint_t) duk_get_length(ctx, -2);
			duk_put_prop_index(ctx, -2, (duk_uarridx_t) len);
			duk_pop(ctx);  /* queue */
		}
	} else if (state == PSTATE_FULFILLED) {
		duk_get_prop_string(ctx, this_idx, STR_P_RESULT);
		duk__pmt_enqueue_reaction(ctx, duk_get_top(ctx) - 1,
		                          handler_fulfill, resolve_cap, reject_cap,
		                          RTYPE_FULFILL);
		duk_pop(ctx);
	} else {
		duk_get_prop_string(ctx, this_idx, STR_P_RESULT);
		duk__pmt_enqueue_reaction(ctx, duk_get_top(ctx) - 1,
		                          handler_reject, resolve_cap, reject_cap,
		                          RTYPE_REJECT);
		duk_pop(ctx);
	}

	/* Pop the two capability fns; the new promise stays. */
	duk_pop_2(ctx);
	return 1;  /* return the new promise */
}

/* ------------------------------------------------------------------ *
 *  Promise.prototype.catch  (=  this.then(undefined, onRejected))     *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__promise_catch(duk_context *ctx) {
	duk_push_this(ctx);
	duk_get_prop_string(ctx, -1, "then");
	duk_insert(ctx, -2);  /* [..., then, this] */
	duk_push_undefined(ctx);
	if (duk_get_top(ctx) >= 4) duk_dup(ctx, 0); else duk_push_undefined(ctx);
	/* call this.then(undefined, onRejected) */
	duk_call_method(ctx, 2);
	return 1;
}

/* ------------------------------------------------------------------ *
 *  Promise.prototype.finally(onFinally)                               *
 * ------------------------------------------------------------------ */

/* JS-level finally: we implement by calling this.then with wrapper
 * functions that invoke onFinally and pass through value/reason. */
static const char duk__promise_finally_src[] =
    "(function finally_(onFinally) {"
    "  if (typeof onFinally !== 'function') return this.then(onFinally, onFinally);"
    "  var C = Promise;"
    "  return this.then("
    "    function(v) { return C.resolve(onFinally()).then(function() { return v; }); },"
    "    function(r) { return C.resolve(onFinally()).then(function() { throw r; }); }"
    "  );"
    "})";

/* ------------------------------------------------------------------ *
 *  Promise.resolve / reject                                           *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__promise_resolve_static(duk_context *ctx) {
	duk_idx_t new_idx;
	/* If x is already our Promise, return it as-is */
	if (duk__pis_promise(ctx, 0)) {
		duk_dup(ctx, 0);
		return 1;
	}
	/* Otherwise construct a new Promise and resolve with x */
	duk_get_global_string(ctx, "Promise");
	duk_get_prop_string(ctx, -1, "prototype");
	duk_push_object(ctx);
	new_idx = duk_get_top(ctx) - 1;
	duk_dup(ctx, -2);  /* prototype */
	duk_set_prototype(ctx, new_idx);
	duk_remove(ctx, -2);  /* prototype */
	duk_remove(ctx, -2);  /* Promise */

	new_idx = duk_get_top(ctx) - 1;
	duk_push_uint(ctx, PSTATE_PENDING);
	duk_put_prop_string(ctx, new_idx, STR_P_STATE);
	duk_push_undefined(ctx);
	duk_put_prop_string(ctx, new_idx, STR_P_RESULT);
	duk_push_array(ctx);
	duk_put_prop_string(ctx, new_idx, STR_P_FQ);
	duk_push_array(ctx);
	duk_put_prop_string(ctx, new_idx, STR_P_RQ);
	duk_push_false(ctx);
	duk_put_prop_string(ctx, new_idx, STR_P_HANDLED);
	duk_push_true(ctx);
	duk_put_prop_string(ctx, new_idx, STR_P_BRAND);

	duk__presolve_promise(ctx, new_idx, 0);
	return 1;
}

static duk_ret_t duk__promise_reject_static(duk_context *ctx) {
	duk_idx_t new_idx;
	duk_get_global_string(ctx, "Promise");
	duk_get_prop_string(ctx, -1, "prototype");
	duk_push_object(ctx);
	new_idx = duk_get_top(ctx) - 1;
	duk_dup(ctx, -2);
	duk_set_prototype(ctx, new_idx);
	duk_remove(ctx, -2);
	duk_remove(ctx, -2);
	new_idx = duk_get_top(ctx) - 1;
	duk_push_uint(ctx, PSTATE_PENDING);
	duk_put_prop_string(ctx, new_idx, STR_P_STATE);
	duk_push_undefined(ctx);
	duk_put_prop_string(ctx, new_idx, STR_P_RESULT);
	duk_push_array(ctx);
	duk_put_prop_string(ctx, new_idx, STR_P_FQ);
	duk_push_array(ctx);
	duk_put_prop_string(ctx, new_idx, STR_P_RQ);
	duk_push_false(ctx);
	duk_put_prop_string(ctx, new_idx, STR_P_HANDLED);
	duk_push_true(ctx);
	duk_put_prop_string(ctx, new_idx, STR_P_BRAND);

	duk__preject_promise(ctx, new_idx, 0);
	return 1;
}

/* ------------------------------------------------------------------ *
 *  Promise.all                                                        *
 * ------------------------------------------------------------------ */

/* JS-side .all implementation -- simpler than open-coding in C.
 * Uses the native Promise constructor so the result has correct shape. */
static const char duk__promise_all_src[] =
    "(function all(iterable) {"
    "  var P = Promise;"
    "  return new P(function(resolve, reject) {"
    "    if (!iterable || typeof iterable.length !== 'number') {"
    "      reject(new TypeError('Promise.all accepts an array')); return;"
    "    }"
    "    var arr = Array.prototype.slice.call(iterable);"
    "    if (arr.length === 0) { resolve([]); return; }"
    "    var results = new Array(arr.length);"
    "    var remaining = arr.length;"
    "    for (var i = 0; i < arr.length; i++) {"
    "      (function(idx) {"
    "        P.resolve(arr[idx]).then(function(v) {"
    "          results[idx] = v;"
    "          if (--remaining === 0) resolve(results);"
    "        }, reject);"
    "      })(i);"
    "    }"
    "  });"
    "})";

static const char duk__promise_race_src[] =
    "(function race(iterable) {"
    "  var P = Promise;"
    "  return new P(function(resolve, reject) {"
    "    if (!iterable || typeof iterable.length !== 'number') {"
    "      reject(new TypeError('Promise.race accepts an array')); return;"
    "    }"
    "    for (var i = 0; i < iterable.length; i++) {"
    "      P.resolve(iterable[i]).then(resolve, reject);"
    "    }"
    "  });"
    "})";

static const char duk__promise_all_settled_src[] =
    "(function allSettled(iterable) {"
    "  var P = Promise;"
    "  return new P(function(resolve, reject) {"
    "    if (!iterable || typeof iterable.length !== 'number') {"
    "      reject(new TypeError('Promise.allSettled accepts an array')); return;"
    "    }"
    "    var arr = Array.prototype.slice.call(iterable);"
    "    if (arr.length === 0) { resolve([]); return; }"
    "    var results = new Array(arr.length);"
    "    var remaining = arr.length;"
    "    for (var i = 0; i < arr.length; i++) {"
    "      (function(idx) {"
    "        P.resolve(arr[idx]).then("
    "          function(v) {"
    "            results[idx] = {status:'fulfilled', value:v};"
    "            if (--remaining === 0) resolve(results);"
    "          },"
    "          function(r) {"
    "            results[idx] = {status:'rejected', reason:r};"
    "            if (--remaining === 0) resolve(results);"
    "          }"
    "        );"
    "      })(i);"
    "    }"
    "  });"
    "})";

static const char duk__promise_any_src[] =
    "(function any(iterable) {"
    "  var P = Promise;"
    "  return new P(function(resolve, reject) {"
    "    if (!iterable || typeof iterable.length !== 'number') {"
    "      reject(new TypeError('Promise.any accepts an array')); return;"
    "    }"
    "    var arr = Array.prototype.slice.call(iterable);"
    "    if (arr.length === 0) {"
    "      var e = new Error('All promises were rejected');"
    "      e.name = 'AggregateError'; e.errors = [];"
    "      reject(e); return;"
    "    }"
    "    var errors = new Array(arr.length);"
    "    var remaining = arr.length;"
    "    for (var i = 0; i < arr.length; i++) {"
    "      (function(idx) {"
    "        P.resolve(arr[idx]).then(resolve, function(r) {"
    "          errors[idx] = r;"
    "          if (--remaining === 0) {"
    "            var e = new Error('All promises were rejected');"
    "            e.name = 'AggregateError'; e.errors = errors;"
    "            reject(e);"
    "          }"
    "        });"
    "      })(i);"
    "    }"
    "  });"
    "})";

/* ------------------------------------------------------------------ *
 *  Microtask job runners                                              *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__pmt_reaction_run(duk_context *ctx, void *udata) {
	duk_rp_microtask *m = (duk_rp_microtask *) udata; /* D9: passed as duk_safe_call udata */
	duk_idx_t base_top = duk_get_top(ctx);
	duk_int_t pcall_rc;
	duk_hobject *cap;

	/* Pass-through (no handler): just feed value to the appropriate cap. */
	if (m->a == NULL) {
		cap = (m->reaction_type == RTYPE_FULFILL) ? m->b : m->c;
		if (cap == NULL) {
			return 0;
		}
		duk_push_hobject(ctx, cap);
		duk_push_tval(ctx, &m->value);
		(void) duk_pcall(ctx, 1);
		duk_set_top(ctx, base_top);
		return 0;
	}

	/* Has handler: call it with value.  pcall leaves return or error on top. */
	duk_push_hobject(ctx, m->a);
	duk_push_tval(ctx, &m->value);
	pcall_rc = duk_pcall(ctx, 1);

	/* Use resolve cap on success, reject cap on error. */
	cap = (pcall_rc == 0) ? m->b : m->c;
	if (cap != NULL) {
		/* Stack: [..., result_or_error].  Call cap(result_or_error). */
		duk_push_hobject(ctx, cap);
		duk_dup(ctx, -2);   /* dup result_or_error */
		(void) duk_pcall(ctx, 1);
	}

	duk_set_top(ctx, base_top);
	return 0;
}

static duk_ret_t duk__pmt_thenable_run(duk_context *ctx, void *udata) {
	duk_rp_microtask *m = (duk_rp_microtask *) udata; /* D9: passed as duk_safe_call udata */
	duk_idx_t base_top = duk_get_top(ctx);
	duk_idx_t promise_idx;

	/* Build resolve/reject for the target promise. */
	duk_push_hobject(ctx, m->a);
	promise_idx = duk_get_top(ctx) - 1;
	duk__pcap_pair_push(ctx, promise_idx);
	/* Stack: [..., promise, resolve, reject] */

	/* Call thenable.then(resolve, reject) protected. */
	duk_push_hobject(ctx, m->c);            /* then function */
	duk_push_hobject(ctx, m->b);            /* thenable (this) */
	duk_dup(ctx, promise_idx + 1);          /* resolve */
	duk_dup(ctx, promise_idx + 2);          /* reject */
	if (duk_pcall_method(ctx, 2) != 0) {
		/* `then` threw synchronously -- call reject(error). */
		duk_dup(ctx, promise_idx + 2);  /* reject */
		duk_dup(ctx, -2);                /* error */
		(void) duk_pcall(ctx, 1);
	}

	duk_set_top(ctx, base_top);
	return 0;
}

/* ------------------------------------------------------------------ *
 *  Public drainer + notifier (declared in duktape.h)                  *
 * ------------------------------------------------------------------ */

static duk_ret_t duk__promise_drain_global(duk_context *ctx) {
	duk_rp_microtask_drain(ctx);
	return 0;
}

DUK_EXTERNAL void duk_rp_microtask_drain(duk_context *ctx) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_rp_mt_queue *q = (duk_rp_mt_queue *) thr->heap->microtask_queue;
	duk_idx_t base_top;
	if (q == NULL) {
		return;
	}
	base_top = duk_get_top(ctx);
	while (q->head != NULL) {
		duk_rp_microtask *m = q->head;
		/* DO NOT dequeue m before running.  The runner may trigger
		 * mark-and-sweep (via any allocation), and duk_rp_microtask_mark
		 * only walks items reachable from q->head.  If m is dequeued
		 * here, m->a/b/c are not marked as roots and can be freed mid-
		 * runner even though we INCREF'd them at enqueue time --
		 * duktape M&S frees unmarked objects regardless of refcount.
		 *
		 * Keeping m at q->head also keeps q->tail valid: if a runner
		 * enqueues a new microtask while m is the only item, the
		 * enqueue path uses q->tail->next, which would dangle if m had
		 * already been dequeued+freed.
		 */
		/* D9: run via duk_safe_call so an OOM-throw during the (unprotected)
		 * setup allocations is caught here instead of longjmping out through the
		 * libevent C frame that drives the drain (no setjmp target -> crash).  On
		 * such a failure the microtask is abandoned (the promise stays unsettled,
		 * as in the pre-existing enqueue-OOM path) and the drain continues.
		 * Normal operation is unaffected. */
		if (m->type == MT_REACTION) {
			(void) duk_safe_call(ctx, duk__pmt_reaction_run, m, 0, 0);
		} else if (m->type == MT_THENABLE) {
			(void) duk_safe_call(ctx, duk__pmt_thenable_run, m, 0, 0);
		}
		/* Now safe to unlink and release. */
		q->head = m->next;
		if (q->head == NULL) {
			q->tail = NULL;
		}
		duk__pmt_release(thr, m);
		/* Defensive: reset stack to base in case a runner left junk */
		duk_set_top(ctx, base_top);
	}
}

DUK_EXTERNAL void duk_rp_microtask_set_notifier(duk_context *ctx,
                                                 void (*cb)(void *udata),
                                                 void *udata) {
	duk_hthread *thr = (duk_hthread *) ctx;
	duk_rp_mt_queue *q = duk__pmt_queue_ensure(thr->heap);
	if (q == NULL) return;
	q->notifier = cb;
	q->notifier_udata = udata;
}

/* ------------------------------------------------------------------ *
 *  GC mark hook + heap teardown                                       *
 * ------------------------------------------------------------------ */

DUK_INTERNAL void duk_rp_microtask_mark(
    duk_heap *heap,
    void (*mark_heaphdr_fn)(duk_heap *, duk_heaphdr *),
    void (*mark_tval_fn)(duk_heap *, duk_tval *)) {
	duk_rp_mt_queue *q = (duk_rp_mt_queue *) heap->microtask_queue;
	duk_rp_microtask *m;
	if (q == NULL) return;
	for (m = q->head; m != NULL; m = m->next) {
		mark_tval_fn(heap, &m->value);
		if (m->a != NULL) mark_heaphdr_fn(heap, (duk_heaphdr *) m->a);
		if (m->b != NULL) mark_heaphdr_fn(heap, (duk_heaphdr *) m->b);
		if (m->c != NULL) mark_heaphdr_fn(heap, (duk_heaphdr *) m->c);
	}
}

DUK_INTERNAL void duk_rp_microtask_queue_free(duk_heap *heap) {
	duk_rp_mt_queue *q = (duk_rp_mt_queue *) heap->microtask_queue;
	if (q == NULL) return;
	while (q->head != NULL) {
		duk_rp_microtask *m = q->head;
		q->head = m->next;
		heap->free_func(heap->heap_udata, m);
	}
	heap->free_func(heap->heap_udata, q);
	heap->microtask_queue = NULL;
}

/* ------------------------------------------------------------------ *
 *  Install function                                                   *
 * ------------------------------------------------------------------ */

DUK_INTERNAL void duk_rp_install_promise_native(duk_context *ctx) {
	/* Construct Promise constructor with property descriptors per spec */
	duk_push_c_function(ctx, duk__promise_ctor, 1);

	/* Set name */
	duk_push_string(ctx, "name");
	duk_push_string(ctx, "Promise");
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE |
	                      DUK_DEFPROP_HAVE_CONFIGURABLE |
	                      DUK_DEFPROP_CONFIGURABLE |
	                      DUK_DEFPROP_FORCE);

	/* Build prototype */
	duk_push_object(ctx);

	duk_push_c_function(ctx, duk__promise_then, 2);
	duk_put_prop_string(ctx, -2, "then");

	duk_push_c_function(ctx, duk__promise_catch, 1);
	duk_put_prop_string(ctx, -2, "catch");

	duk_eval_string(ctx, duk__promise_finally_src);
	duk_put_prop_string(ctx, -2, "finally");

	/* prototype.constructor = Promise */
	duk_dup(ctx, -2);
	duk_put_prop_string(ctx, -2, "constructor");

	/* Promise.prototype = prototype */
	duk_put_prop_string(ctx, -2, "prototype");

	/* Static methods */
	duk_push_c_function(ctx, duk__promise_resolve_static, 1);
	duk_put_prop_string(ctx, -2, "resolve");
	duk_push_c_function(ctx, duk__promise_reject_static, 1);
	duk_put_prop_string(ctx, -2, "reject");
	duk_eval_string(ctx, duk__promise_all_src);
	duk_put_prop_string(ctx, -2, "all");
	duk_eval_string(ctx, duk__promise_race_src);
	duk_put_prop_string(ctx, -2, "race");
	duk_eval_string(ctx, duk__promise_all_settled_src);
	duk_put_prop_string(ctx, -2, "allSettled");
	duk_eval_string(ctx, duk__promise_any_src);
	duk_put_prop_string(ctx, -2, "any");

	/* Install as global Promise (overwrites stub or polyfill). */
	duk_put_global_string(ctx, "Promise");

	/* Expose drainer as a global helper for testing and for embedders
	 * that prefer a JS-callable handle (the C API is duk_rp_microtask_drain). */
	duk_push_c_function(ctx, duk__promise_drain_global, 0);
	duk_put_global_string(ctx, "__drainMicrotasks");
}

#endif  /* DUK_RP_USE_PROMISE_NATIVE */
