/*
 *  Rampart contributions on top of duktape 2.7.0.
 *
 *  Every DUK_RP_USE_* flag gates a discrete, optional feature added
 *  by rampart and never present in upstream duktape.  By default all
 *  flags are off -- the regenerated duktape.c then behaves exactly as
 *  upstream 2.7.0.  Define DUK_RP_ALL (or individual flags) before
 *  including duktape.h, or pass -DDUK_RP_ALL on the compiler line, to
 *  turn the rampart features on.
 *
 *  Two derived guards are emitted automatically below:
 *    DUK_RP_ANY_RUNTIME   -- at least one runtime feature is enabled
 *                            (Promise, Map/Set, iterators, ES polyfills,
 *                            Blob, etc.).  Used in heap-create to wire
 *                            in duk_rp_install_extensions().
 *    DUK_RP_ANY_INTERNAL  -- at least one feature that only changes
 *                            duktape's own C/internal behaviour is on
 *                            (cancel/timeout, force_interrupt,
 *                            get_own_prop_info, scope_vars).
 */

#if defined(DUK_RP_ALL)
/* === Runtime features (install at heap-create) === */
#define DUK_RP_USE_PROMISE                 /* Promise polyfill (ES2015) */
#define DUK_RP_USE_MAP_SET                 /* Map, Set (ES2015) */
#define DUK_RP_USE_TEXTENCODING            /* TextEncoder, TextDecoder (WHATWG) */
#define DUK_RP_USE_BLOB                    /* Blob, File (W3C File API) */
#define DUK_RP_USE_BUFFER_EXTRAS           /* node-compat extras on duktape Buffer */
#define DUK_RP_USE_CONSOLE_EXTENDED        /* console.time/group/count/table/clear */
#define DUK_RP_USE_ARRAY_ITER              /* Array.prototype keys/values/entries/[Symbol.iterator] */
#define DUK_RP_USE_STRING_ITER             /* String.prototype[Symbol.iterator] (surrogate-pair aware) */
#define DUK_RP_USE_ASYNC_ITER_SYMBOL       /* Symbol.asyncIterator well-known symbol (ES2018) */
#define DUK_RP_USE_PROXY_REVOCABLE         /* Proxy.revocable (ES2015) */
#define DUK_RP_USE_OBJECT_VALUES_ENTRIES   /* Object.values, Object.entries (ES2017) */
#define DUK_RP_USE_ARRAY_EXTRAS            /* Array.{from,of,prototype.{includes,flat,flatMap,at,findLast,findLastIndex}} */
#define DUK_RP_USE_STRING_EXTRAS           /* String.prototype.{trimStart,trimEnd,replaceAll} */
#define DUK_RP_USE_OBJECT_EXTRAS           /* Object.{hasOwn,fromEntries} (ES2022) */
#define DUK_RP_USE_MODERN_POLYFILLS        /* Object.groupBy, String.prototype.matchAll */
#define DUK_RP_USE_BIGINT                  /* ES2020 BigInt (libtommath-backed) */
#define DUK_RP_USE_TYPEDARRAY_EXTRAS       /* %TypedArray%.prototype[@@toStringTag] + future TA spec polish */
#define DUK_RP_USE_WEAK_REFS               /* WeakRef, WeakMap, WeakSet, FinalizationRegistry (ES2021) */
#define DUK_RP_USE_PROMISE_NATIVE          /* Native C Promise (replaces DUK_RP_USE_PROMISE polyfill) */
/* === Internal / C-API features (compile-time gates only) === */
#define DUK_RP_USE_SCOPE_VARS              /* duk_rp_get_scope_vars C API */
#define DUK_RP_USE_CANCEL                  /* duk_cancel + LJ_TYPE_RETURN silent unwind */
#define DUK_RP_USE_FORCE_INTERRUPT         /* duk_force_interrupt C API */
#define DUK_RP_USE_OWN_PROP_INFO           /* duk_get_own_prop_info C API */
#endif

#if defined(DUK_RP_USE_PROMISE) || defined(DUK_RP_USE_MAP_SET) || \
    defined(DUK_RP_USE_TEXTENCODING) || defined(DUK_RP_USE_BLOB) || \
    defined(DUK_RP_USE_BUFFER_EXTRAS) || defined(DUK_RP_USE_CONSOLE_EXTENDED) || \
    defined(DUK_RP_USE_ARRAY_ITER) || defined(DUK_RP_USE_STRING_ITER) || \
    defined(DUK_RP_USE_ASYNC_ITER_SYMBOL) || defined(DUK_RP_USE_PROXY_REVOCABLE) || \
    defined(DUK_RP_USE_OBJECT_VALUES_ENTRIES) || defined(DUK_RP_USE_ARRAY_EXTRAS) || \
    defined(DUK_RP_USE_STRING_EXTRAS) || defined(DUK_RP_USE_OBJECT_EXTRAS) || \
    defined(DUK_RP_USE_MODERN_POLYFILLS) || defined(DUK_RP_USE_BIGINT) || \
    defined(DUK_RP_USE_TYPEDARRAY_EXTRAS) || defined(DUK_RP_USE_WEAK_REFS) || \
    defined(DUK_RP_USE_PROMISE_NATIVE)
#define DUK_RP_ANY_RUNTIME
#endif

#if defined(DUK_RP_USE_SCOPE_VARS) || defined(DUK_RP_USE_CANCEL) || \
    defined(DUK_RP_USE_FORCE_INTERRUPT) || defined(DUK_RP_USE_OWN_PROP_INFO)
#define DUK_RP_ANY_INTERNAL
#endif

#if defined(DUK_RP_USE_CANCEL)
/* The cancel/timeout path uses pthread_testcancel inside the bytecode
 * loop; pull in the pthread declarations.  Embedders must link with
 * -lpthread when this flag is on. */
#include <pthread.h>
/* Hook for the embedder's exec-timeout check.  The embedder supplies
 * rp_cancel_check(udata) and rp_cancel_disarm() functions that return
 * 0 (continue), 1 (RangeError), or 2 (silent unwind via LJ_TYPE_RETURN). */
extern int rp_cancel_check(void *udata);
extern void rp_cancel_disarm(void);
#if !defined(DUK_USE_EXEC_TIMEOUT_CHECK)
#define DUK_USE_EXEC_TIMEOUT_CHECK(udata) rp_cancel_check(udata)
#endif
#if !defined(DUK_USE_EXEC_TIMEOUT_DISARM)
#define DUK_USE_EXEC_TIMEOUT_DISARM() rp_cancel_disarm()
#endif
/* The interrupt counter must be enabled for the exec-timeout to fire. */
#if !defined(DUK_USE_INTERRUPT_COUNTER)
#define DUK_USE_INTERRUPT_COUNTER
#endif
#endif  /* DUK_RP_USE_CANCEL */

#if defined(DUK_RP_USE_FORCE_INTERRUPT)
/* duk_force_interrupt requires the interrupt counter mechanism. */
#if !defined(DUK_USE_INTERRUPT_COUNTER)
#define DUK_USE_INTERRUPT_COUNTER
#endif
#endif
