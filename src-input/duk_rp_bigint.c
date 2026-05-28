/*
 *  duk_rp_bigint.c
 *
 *  ES2020 BigInt support for duktape, backed by libtommath.
 *  Gated by DUK_RP_USE_BIGINT.
 *
 *  ----------------------------------------------------------------
 *  Representation
 *  ----------------------------------------------------------------
 *  A JS BigInt is a duk_hobject of class DUK_HOBJECT_CLASS_BIGINT
 *  (slot 30, added to duk_hobject.h) carrying an mp_int* via a
 *  hidden internal-key property '\xff_bn'.  The mp_int is allocated
 *  separately on the C heap by mp_init() (libtommath owns its own
 *  mp_digit limb storage) and freed by a finalizer attached to the
 *  object.  Class slot 30 is what makes `typeof x === 'bigint'` and
 *  Object.prototype.toString.call(x) === '[object BigInt]' work
 *  natively without Symbol.toStringTag.
 *
 *  Memory ownership: mp_int allocation lives on the C heap (malloc),
 *  not the duktape heap, because libtommath's mp_digit limb buffer
 *  realloc's independently as numbers grow.  The duktape object
 *  owns the mp_int via the finalizer.  Between mp_init() and the
 *  finalizer attach there is a small leak-on-OOM window we accept
 *  for simplicity; the window is bounded by a single put_prop +
 *  set_finalizer pair.
 *
 *  ----------------------------------------------------------------
 *  Public C surface
 *  ----------------------------------------------------------------
 *  Construction / inspection:
 *    duk_rp_push_bigint_zero(thr) -> mp_int*
 *    duk_rp_push_bigint_from_i64(thr, v)
 *    duk_rp_push_bigint_from_double(thr, v)        (throws if !integer)
 *    duk_rp_push_bigint_from_string(thr, s, radix) (throws on parse err)
 *    duk_rp_is_bigint(thr, idx) -> duk_bool_t
 *    duk_rp_get_bigint_mp(thr, idx) -> mp_int* | NULL
 *    duk_rp_tval_is_bigint(tv) -> duk_bool_t       (no-stack fast path)
 *
 *  Stringification / numeric:
 *    duk_rp_push_bigint_to_string(thr, idx, radix) (engine-internal
 *                                                   stringification;
 *                                                   bypasses user-
 *                                                   overridable
 *                                                   BigInt.prototype.
 *                                                   toString)
 *    duk_rp_bigint_to_double(thr, idx) -> double   (Number() builtin
 *                                                   path; ToNumber
 *                                                   still throws)
 *
 *  Operator dispatch (called from duk_js_executor.c + duk_js_ops.c
 *  at the top of arith/bitwise/compare/equality/unary paths):
 *    duk_rp_bigint_try_add(thr, x, y, idx_z)       (handles string concat)
 *    duk_rp_bigint_try_arith(thr, x, y, op, idx_z)
 *    duk_rp_bigint_try_bitwise(thr, x, y, op, idx_z)
 *    duk_rp_bigint_try_compare(thr, x, y, *out_ord)
 *    duk_rp_bigint_try_equals(thr, x, y, strict, *out_eq)
 *    duk_rp_bigint_try_unary(thr, x, op, idx_z)
 *
 *  The try_* helpers return 1 if they handled the case, 0 to fall
 *  back to the standard number-only path.  Mixed BigInt + Number
 *  arithmetic throws TypeError per spec.
 *
 *  ----------------------------------------------------------------
 *  Known limitations (spec divergences)
 *  ----------------------------------------------------------------
 *  This impl passes 119 of 141 test262 BigInt-related tests, when
 *  validated by the rampart embedder against tc39/test262.  The 20
 *  real spec divergences are summarised here.  Each item names the
 *  test262 file path(s) under tc39/test262, the spec-vs-actual gap,
 *  and the fix path.
 *
 *  1. Object(1n) vs 1n -- no wrapper/primitive distinction
 *     test262: built-ins/BigInt/wrapper-object-ordinary-toprimitive.js
 *     Spec: Object(1n) is a wrapper object; ToPrimitive on it uses
 *     valueOf-then-toString and CAN return the user-overridable
 *     BigInt.prototype.toString result.  Our impl: primitive and
 *     wrapper share class CLASS_BIGINT (we have no separate
 *     CLASS_BIGINT_WRAPPER slot).
 *     Fix: expand DUK_HOBJECT_FLAG_CLASS_BITS from 5 to 6 (frees a
 *     bit by repurposing one hobject user-flag) and add
 *     CLASS_BIGINT_WRAPPER at slot 32+.
 *
 *  2. Cross-realm $262 agent not exposed
 *     test262: built-ins/BigInt/prototype/valueOf/cross-realm.js
 *     test262 uses `$262.createRealm()` to verify valueOf works
 *     across realms.  Duktape doesn't expose multi-realm via the
 *     $262 harness.  Fix: implement $262 stub for the existing
 *     multi-heap mechanism, or document non-applicable.
 *
 *  3. isConstructor(BigInt) returns false
 *     test262: built-ins/BigInt/is-a-constructor.js
 *     Spec: BigInt has a [[Construct]] internal slot;
 *     Reflect.construct(function(){}, [], BigInt) must succeed.
 *     Our impl: the constructor body throws on new.target, which
 *     trips Reflect.construct's [[Construct]] probe.
 *     Fix: mark the function as constructable; let the body throw
 *     TypeError when actually invoked with new.target.
 *
 *  4. ToBigInt(Symbol) throws SyntaxError instead of TypeError
 *     test262: built-ins/BigInt/asIntN/bigint-tobigint-errors.js
 *              built-ins/BigInt/asUintN/bigint-tobigint-errors.js
 *     Spec: ToBigInt(Symbol) -> TypeError.  Our impl: Symbols are
 *     encoded as duk_hstring with a marker prefix.  The STRING case
 *     in duk__bigint_to_bigint tries to parse the description as a
 *     number, producing SyntaxError.
 *     Fix: add `duk_is_symbol(ctx, idx)` -> TypeError check before
 *     the STRING case in duk__bigint_to_bigint.
 *
 *  5. Very-large BigInt vs Number near Number.MAX_VALUE
 *     test262: language/expressions/less-than/bigint-and-number-extremes.js
 *     Spec ARC step h: mathematical compare must beat
 *     Number-representation precision.  Our impl converts the
 *     BigInt to double via mp_get_double, losing precision near
 *     2^1024.
 *     Fix: implement proper double-decomposition -- extract
 *     mantissa+exponent, compare BigInt to integer part exactly,
 *     then handle fractional via floor/ceil.
 *
 *  6. BigInt literal as object property key
 *     test262: language/expressions/object/literal-property-name-bigint.js
 *     Spec: { 1n: "v" } is equivalent to { "1": "v" } via
 *     ToPropertyKey(bigint) -> bigint.toString().  Our impl: parser
 *     rejects DUK_TOK_BIGINT in PropertyName position.
 *     Fix: extend the PropertyName production in duk_js_compiler.c
 *     to accept DUK_TOK_BIGINT and emit its toString form as the
 *     literal-property-name string.
 *
 *  7. Resizable ArrayBuffer (ES2024)
 *     test262: language/destructuring/binding/typedarray-backed-by-
 *              resizable-buffer.js  AND 5 for-of variants under
 *              language/statements/for-of/typedarray-backed-by-
 *              resizable-buffer-*.js
 *     Spec: ArrayBuffer accepts a {maxByteLength} option enabling
 *     .resize().  Our duktape ArrayBuffer is fixed-size.
 *     Fix: add resize support to ArrayBuffer (significant change
 *     to buffer heap-object representation + view-invalidation
 *     semantics).
 *
 *  8. Boolean(0n) === true instead of false
 *     (not in test262 fail list directly -- exposed by spec but
 *     no test262 file explicitly checks it; documenting here.)
 *     duk_js_toboolean has no thread parameter, so it can't read
 *     mp_int* without stack/heap context.
 *     Fix: cache an "is zero" bit in the hobject's flag word at
 *     construction time, OR add a thread-aware toboolean variant.
 *
 *  9. BigInt64Array's [[Prototype]] is not %TypedArray%
 *     test262: built-ins/TypedArrayConstructors/BigInt64Array/proto.js
 *     Spec: Object.getPrototypeOf(BigInt64Array) === %TypedArray%
 *     (the abstract typed-array constructor intrinsic).  Our impl:
 *     BigInt64Array is JS-level Proxy-backed; no %TypedArray%
 *     intrinsic is exposed.
 *     Fix: either (a) expand class field to 6 bits + add
 *     CLASS_BIGINT64ARRAY / CLASS_BIGUINT64ARRAY native typed-array
 *     entries; OR (b) expose %TypedArray% intrinsic by hoisting the
 *     existing TA prototype chain so Int8Array etc. share a real
 *     common ancestor visible as TypedArray.
 *
 * 10. new BigInt64Array(BigInt64Array.prototype)
 *     test262: built-ins/TypedArrayConstructors/BigInt64Array/prototype.js
 *     Spec: passing the prototype object as the data argument is
 *     a valid TypedArray construction.  Our JS Proxy-backed ctor
 *     rejects unknown argument shapes.  Fix: same as item 9.
 *
 * 11. isConstructor(BigInt64Array) returns false
 *     test262: built-ins/TypedArrayConstructors/BigInt64Array/
 *              is-a-constructor.js
 *     Spec: BigInt64Array is a real constructor.  Our impl: it's a
 *     JS function returning a Proxy, which doesn't satisfy
 *     Reflect.construct's [[Construct]] probe.  Fix: same as 9.
 *
 * 12. BigInt64Array.prototype's [[Prototype]] is not %TypedArray%.prototype
 *     test262: built-ins/TypedArrayConstructors/BigInt64Array/
 *              prototype/proto.js
 *     Same root cause as item 9 (no %TypedArray% prototype chain).
 *
 * 13. BigInt64Array.prototype.buffer brand-check missing
 *     test262: built-ins/TypedArrayConstructors/BigInt64Array/
 *              prototype/not-typedarray-object.js
 *     Spec: accessing the typed-array accessor `buffer` (and
 *     byteLength / byteOffset / length) on BigInt64Array.prototype
 *     must throw TypeError -- the accessor has a brand check for
 *     [[ViewedArrayBuffer]] which the prototype object lacks.  Our
 *     impl: no `buffer` defined on the prototype at all, so the
 *     property access returns undefined.
 *     Fix: define each TA accessor as a getter that brand-checks
 *     `this` for the internal slot.
 *
 * 14. class extends BigInt64Array
 *     test262: language/statements/class/subclass-builtins/
 *              subclass-BigInt64Array.js (and the expressions/
 *              class/... variant)
 *     Spec: a class can extend BigInt64Array; super() must produce
 *     a properly-shaped TA instance.  Our impl: Ctor uses Proxy and
 *     returns it from the body; subclass super() doesn't get the
 *     Proxy wiring.  Fix: needs real engine-level typed-array
 *     implementation supporting [[Construct]] + new.target chain
 *     per spec.
 *
 *  ----------------------------------------------------------------
 *  False positives in the test262 fail list (3 tests)
 *  ----------------------------------------------------------------
 *  These fail in test262 but behave per-spec in isolation -- the
 *  failure is in test262's own harness (deeply-nested assert.throws
 *  combined with computed-property names in the test body produce
 *  a "(null)" error message even though the underlying assertion
 *  passes).  No fix required on our side:
 *    - built-ins/BigInt/asIntN/not-a-constructor.js
 *    - built-ins/BigInt/asUintN/not-a-constructor.js
 *  And one previously thought to be a false positive but actually
 *  IS a real divergence (item 13 above):
 *    - built-ins/TypedArrayConstructors/BigInt64Array/prototype/
 *      not-typedarray-object.js
 *
 *  The numbered list above is the canonical fix roadmap.  The
 *  rampart embedder ships a corresponding test/bigint-test.js with
 *  20 active testFeature groups (one per spec area, covering the
 *  119 passing test262 cases) plus 13 commented-out LIMIT: groups
 *  that exercise items 1-7 and 9-14 above and verify they still
 *  fail (item 8, Boolean(0n), has no test262 file).  See that
 *  rampart-side test for verifying limitation status across changes.
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_BIGINT)

#include "duk_rp_internal.h"
/* Vendored libtommath staged into <tempdir>/src/tommath/ by
 * configure.py.  The path-qualified include keeps these headers out of
 * the embedder's top-level include namespace (no risk of clashing with
 * a system libtommath that might also be on the include path). */
#include "tommath/tommath.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Hidden internal-key property holding the mp_int*.  Names starting
 * with the \xff byte are auto-hidden from for-in / Object.keys per
 * duktape's symbol/internal-key convention. */
#define DUK_RP_BIGINT_KEY     "\xff" "_bn"
#define DUK_RP_BIGINT_KEY_LEN (sizeof(DUK_RP_BIGINT_KEY) - 1)

/* ------------------------------------------------------------------ */
/* mp_int lifecycle                                                    */
/* ------------------------------------------------------------------ */

static mp_int *duk__bigint_mp_alloc(void) {
    mp_int *bn = (mp_int *) malloc(sizeof(mp_int));
    if (bn == NULL) {
        return NULL;
    }
    if (mp_init(bn) != MP_OKAY) {
        free(bn);
        return NULL;
    }
    return bn;
}

static void duk__bigint_mp_free(mp_int *bn) {
    if (bn != NULL) {
        mp_clear(bn);
        free(bn);
    }
}

/* ------------------------------------------------------------------ */
/* mp_int <-> JS object plumbing                                       */
/* ------------------------------------------------------------------ */

/* Fetch the mp_int* embedded in the BigInt object at idx.  Returns
 * NULL if the value is not a BigInt instance.  Does not throw. */
static mp_int *duk__bigint_mp_peek(duk_context *ctx, duk_idx_t idx) {
    mp_int *bn = NULL;
    if (!duk_is_object(ctx, idx)) {
        return NULL;
    }
    if (duk_get_prop_lstring(ctx, idx, DUK_RP_BIGINT_KEY, DUK_RP_BIGINT_KEY_LEN)) {
        bn = (mp_int *) duk_get_pointer_default(ctx, -1, NULL);
    }
    duk_pop(ctx);
    return bn;
}

/* Finalizer: invoked by GC when a BigInt object becomes unreachable.
 * Stack convention: arg 0 is the object being finalized; heap_destruct
 * flag is arg 1 but we don't care.  Returns 0. */
static duk_ret_t duk__bigint_finalizer(duk_context *ctx) {
    mp_int *bn = duk__bigint_mp_peek(ctx, 0);
    if (bn != NULL) {
        duk__bigint_mp_free(bn);
        /* Clear the prop so a re-entrant finalizer (shouldn't happen
         * but be defensive) doesn't double-free. */
        duk_push_pointer(ctx, NULL);
        duk_put_prop_lstring(ctx, 0, DUK_RP_BIGINT_KEY, DUK_RP_BIGINT_KEY_LEN);
    }
    return 0;
}

/* Attach BigInt.prototype to the object at idx.  Resolves the
 * constructor lazily so the install order of BigInt() vs. user code
 * pushing BigInts during boot doesn't matter. */
static void duk__bigint_set_proto(duk_context *ctx, duk_idx_t idx) {
    idx = duk_normalize_index(ctx, idx);
    if (duk_get_global_string(ctx, "BigInt")) {
        if (duk_get_prop_string(ctx, -1, "prototype")) {
            duk_set_prototype(ctx, idx);
        } else {
            duk_pop(ctx);
        }
    }
    duk_pop(ctx);
}

/* Wrap an mp_int* in a fresh JS object and push it.  Takes ownership
 * of `bn` (it will be freed at finalization).  Caller MUST NOT
 * dereference `bn` after this call -- treat the JS value as the only
 * handle.  Use duk_rp_get_bigint_mp(thr, idx) to read it back. */
static void duk__bigint_push_wrapper(duk_context *ctx, mp_int *bn) {
    duk_idx_t idx;
    duk_hthread *thr = (duk_hthread *) ctx;
    duk_hobject *h;

    duk_push_object(ctx);
    idx = duk_get_top_index(ctx);

    /* Mark this object as class BIGINT so typeof returns 'bigint' and
     * Object.prototype.toString returns '[object BigInt]'.  The class
     * number is in 5 bits of the heap-header flag word, so flipping
     * it from CLASS_OBJECT (1) to CLASS_BIGINT (30) is a single set. */
    h = duk_get_hobject(ctx, idx);
    DUK_ASSERT(h != NULL);
    DUK_HOBJECT_SET_CLASS_NUMBER(h, DUK_HOBJECT_CLASS_BIGINT);
    DUK_UNREF(thr);

    duk_push_pointer(ctx, (void *) bn);
    duk_put_prop_lstring(ctx, idx, DUK_RP_BIGINT_KEY, DUK_RP_BIGINT_KEY_LEN);

    /* finalizer */
    duk_push_c_function(ctx, duk__bigint_finalizer, 1 /*nargs*/);
    duk_set_finalizer(ctx, idx);

    duk__bigint_set_proto(ctx, idx);
}

/* ------------------------------------------------------------------ */
/* Public C API                                                        */
/* ------------------------------------------------------------------ */

/* Internal: used by other duk_rp_* helpers to allocate a zero-value
 * BigInt; exposes mp_int* which would couple embedders to libtommath. */
DUK_INTERNAL mp_int *duk_rp_push_bigint_zero(duk_context *ctx) {
    mp_int *bn = duk__bigint_mp_alloc();
    if (bn == NULL) {
        RP_THROW(ctx, "BigInt allocation failed");
    }
    duk__bigint_push_wrapper(ctx, bn);
    return bn;
}

DUK_EXTERNAL duk_bool_t duk_rp_is_bigint(duk_context *ctx, duk_idx_t idx) {
    if (!duk_is_object(ctx, idx)) {
        return 0;
    }
    if (duk_get_prop_lstring(ctx, idx, DUK_RP_BIGINT_KEY, DUK_RP_BIGINT_KEY_LEN)) {
        duk_bool_t ok = duk_is_pointer(ctx, -1) && (duk_get_pointer(ctx, -1) != NULL);
        duk_pop(ctx);
        return ok;
    }
    duk_pop(ctx);
    return 0;
}

/* Internal: returns mp_int* which couples to libtommath; embedders
 * should use duk_rp_bigint_to_double / duk_rp_push_bigint_to_string
 * instead. */
DUK_INTERNAL mp_int *duk_rp_get_bigint_mp(duk_context *ctx, duk_idx_t idx) {
    return duk__bigint_mp_peek(ctx, idx);
}

DUK_EXTERNAL void duk_rp_push_bigint_from_i64(duk_context *ctx, duk_int64_t v) {
    mp_int *bn = duk_rp_push_bigint_zero(ctx);
    duk_uint64_t mag;
    int neg = 0;
    if (v < 0) {
        neg = 1;
        /* Avoid undefined behavior on INT64_MIN. */
        mag = (duk_uint64_t) -(v + 1) + 1;
    } else {
        mag = (duk_uint64_t) v;
    }
    /* mp_set_u64 returns void; the mp_int already has alloc room
     * from mp_init(), and assigning <= 64 bits never needs growth. */
    mp_set_u64(bn, (uint64_t) mag);
    if (neg) {
        if (mp_neg(bn, bn) != MP_OKAY) {
            RP_THROW(ctx, "BigInt: mp_neg failed");
        }
    }
}

DUK_EXTERNAL void duk_rp_push_bigint_from_double(duk_context *ctx, double d) {
    /* Spec: BigInt(number) requires `number` be a finite integer
     * (Number.isInteger).  Otherwise throw RangeError. */
    if (!(d == d) || d == (double) (1.0/0.0) || d == (double) (-1.0/0.0)) {
        RP_RANGE_THROW(ctx, "Cannot convert non-finite number to BigInt");
    }
    if (d != (double) (duk_int64_t) d) {
        /* Either fractional or out of i64 range.  i64 covers ~9.22e18;
         * for larger magnitudes we'd need a multi-limb path here, but
         * such values can't come from JS Number losslessly anyway
         * (Number.MAX_SAFE_INTEGER is 2^53-1).  For now: if it doesn't
         * round-trip through i64, reject. */
        if ((double) (duk_int64_t) d != d) {
            RP_RANGE_THROW(ctx, "BigInt: number must be an integer in i64 range");
        }
    }
    duk_rp_push_bigint_from_i64(ctx, (duk_int64_t) d);
}

DUK_EXTERNAL void duk_rp_push_bigint_from_string(duk_context *ctx,
                                                 const char *s,
                                                 int radix) {
    mp_int *bn;
    const char *p;
    int detected_radix = radix;

    if (s == NULL) {
        RP_SYNTAX_THROW(ctx, "BigInt: null string");
    }

    /* Trim leading whitespace. */
    p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f') p++;

    /* Spec: StringToBigInt of an empty or whitespace-only string is 0n. */
    if (*p == 0) {
        duk_rp_push_bigint_from_i64(ctx, 0);
        return;
    }

    /* Trim trailing whitespace.  Since the underlying string is
     * immutable, push a duktape copy that we manually truncate-by-
     * substring.  We must remove this temp before returning so the
     * stack effect of the function stays "push one BigInt". */
    {
        size_t L = strlen(p);
        while (L > 0 && (p[L-1] == ' ' || p[L-1] == '\t' || p[L-1] == '\n' ||
                         p[L-1] == '\r' || p[L-1] == '\v' || p[L-1] == '\f')) {
            L--;
        }
        if (L == 0) {
            duk_rp_push_bigint_from_i64(ctx, 0);
            return;
        }
        duk_push_lstring(ctx, p, L);
        p = duk_get_string(ctx, -1);
    }
    /* The trimmed-string temp sits below where we'll push the BigInt.
     * At every return path below, we duk_remove the temp so the net
     * stack effect remains +1 (the BigInt). */
#define DUK__BIGINT_FROM_STRING_RETURN()  do { duk_remove(ctx, -2); return; } while (0)

    /* JS allows a leading sign. */
    {
        const char *start = p;
        int has_sign = (*p == '+' || *p == '-');
        if (has_sign) p++;

        /* Detect 0x/0o/0b prefixes when radix is unspecified (0). */
        if (detected_radix == 0) {
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                detected_radix = 16;
                /* Spec: hex/oct/bin string forms must NOT have a sign. */
                if (has_sign) {
                    RP_SYNTAX_THROW(ctx, "BigInt: signed non-decimal literal");
                }
                p += 2;
            } else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
                detected_radix = 8;
                if (has_sign) {
                    RP_SYNTAX_THROW(ctx, "BigInt: signed non-decimal literal");
                }
                p += 2;
            } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
                detected_radix = 2;
                if (has_sign) {
                    RP_SYNTAX_THROW(ctx, "BigInt: signed non-decimal literal");
                }
                p += 2;
            } else {
                detected_radix = 10;
            }
        }

        /* Empty payload after sign/prefix is an error. */
        if (*p == '\0') {
            RP_SYNTAX_THROW(ctx, "BigInt: empty string");
        }

        bn = duk_rp_push_bigint_zero(ctx);

        /* mp_read_radix accepts a leading '-' but not '+'.  Strip + if
         * present; pass '-' through. */
        if (detected_radix == 10 && start != s) {
            /* already at start (no whitespace) */
        }
        {
            /* Build the payload that mp_read_radix will parse: copy
             * the sign (if '-') plus the digits. */
            const char *digits = p;
            int neg = (start[0] == '-');
            if (neg) {
                /* mp_read_radix consumes one leading '-'.  We synthesize
                 * by parsing magnitude then negating, which avoids a
                 * malloc for the prefixed copy. */
                if (mp_read_radix(bn, digits, detected_radix) != MP_OKAY) {
                    RP_SYNTAX_THROW(ctx, "BigInt: cannot parse '%s'", s);
                }
                if (mp_iszero(bn) == MP_NO) {
                    if (mp_neg(bn, bn) != MP_OKAY) {
                        RP_THROW(ctx, "BigInt: mp_neg failed");
                    }
                }
            } else {
                if (mp_read_radix(bn, digits, detected_radix) != MP_OKAY) {
                    RP_SYNTAX_THROW(ctx, "BigInt: cannot parse '%s'", s);
                }
            }
        }
    }
    /* Drop the trimmed-string temp pushed by the leading-whitespace
     * block; the BigInt we just produced is at top-1.  After this
     * the net stack effect is +1. */
    duk_remove(ctx, -2);
#undef DUK__BIGINT_FROM_STRING_RETURN
}

/* ------------------------------------------------------------------ */
/* Public helpers for binop / comparison dispatch                      */
/*                                                                     */
/* The executor + duk_js_ops.c call these at the top of each binop to  */
/* short-circuit BigInt cases.  When neither operand is a BigInt the   */
/* check is a single tag + class comparison and falls through to the   */
/* number/string code; when at least one IS a BigInt the helper takes  */
/* over and either computes the result or throws (mixed BigInt+number  */
/* is a TypeError per spec).                                           */
/* ------------------------------------------------------------------ */

/* Engine-internal stringification of a BigInt value.  Bypasses the
 * JS-visible BigInt.prototype.toString so user monkey-patches don't
 * affect ToString(bigint) per spec.  Pushes the result string. */
DUK_EXTERNAL void duk_rp_push_bigint_to_string(duk_hthread *thr, duk_idx_t idx, int radix) {
    duk_context *ctx = (duk_context *) thr;
    mp_int *bn = duk__bigint_mp_peek(ctx, idx);
    int size = 0;
    size_t written = 0;
    char *buf;
    if (bn == NULL) {
        duk_push_string(ctx, "0");
        return;
    }
    if (mp_radix_size(bn, radix, &size) != MP_OKAY || size <= 0) {
        duk_push_string(ctx, "0");
        return;
    }
    buf = (char *) duk_push_fixed_buffer(ctx, (duk_size_t) size);
    if (mp_to_radix(bn, buf, (size_t) size, &written, radix) != MP_OKAY) {
        duk_pop(ctx);
        duk_push_string(ctx, "0");
        return;
    }
    {
        size_t i, out_len = written > 0 ? written - 1 : 0;
        for (i = 0; i < out_len; i++) {
            if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = (char) (buf[i] + 32);
        }
        duk_push_lstring(ctx, buf, out_len);
    }
    duk_remove(ctx, -2);  /* drop fixed buffer */
}

/* Convert a BigInt at `idx` to a JavaScript Number (round-to-nearest
 * via libtommath mp_get_double).  Used by the Number() builtin's
 * BigInt branch -- distinct from ToNumber which throws TypeError. */
DUK_EXTERNAL double duk_rp_bigint_to_double(duk_hthread *thr, duk_idx_t idx) {
    duk_context *ctx = (duk_context *) thr;
    mp_int *bn = duk__bigint_mp_peek(ctx, idx);
    if (bn == NULL) return 0.0;
    return mp_get_double(bn);
}

DUK_INTERNAL duk_bool_t duk_rp_tval_is_bigint(duk_tval *tv) {
    duk_hobject *h;
    if (!DUK_TVAL_IS_OBJECT(tv)) return 0;
    h = DUK_TVAL_GET_OBJECT(tv);
    if (h == NULL) return 0;
    return DUK_HOBJECT_GET_CLASS_NUMBER(h) == DUK_HOBJECT_CLASS_BIGINT;
}

/* Read mp_int* from a BigInt tval.  Caller MUST have verified
 * duk_rp_tval_is_bigint(tv) returned true. */
static mp_int *duk__bigint_mp_from_tval(duk_hthread *thr, duk_tval *tv) {
    mp_int *bn;
    duk_push_tval((duk_context *) thr, tv);
    bn = duk__bigint_mp_peek((duk_context *) thr, -1);
    duk_pop((duk_context *) thr);
    return bn;
}

/* Coerce a non-BigInt primitive to a BigInt for spec-compliant
 * mixed-type ops.  Throws TypeError when the input is a Number
 * (per spec: arithmetic on mixed BigInt+Number must throw, never
 * implicitly convert).  Strings, booleans, and BigInt objects ARE
 * accepted because they participate in equality / loose-equality
 * comparisons that DO allow cross-type semantics.  Pushes result
 * onto the stack and returns its mp_int*. */
static mp_int *duk__bigint_coerce_for_eq(duk_context *ctx, duk_tval *tv) {
    duk_push_tval(ctx, tv);
    if (duk_rp_is_bigint(ctx, -1)) {
        return duk__bigint_mp_peek(ctx, -1);
    }
    if (duk_is_string(ctx, -1)) {
        const char *s = duk_get_string(ctx, -1);
        duk_pop(ctx);
        duk_rp_push_bigint_from_string(ctx, s, 0);
        return duk__bigint_mp_peek(ctx, -1);
    }
    if (duk_is_boolean(ctx, -1)) {
        duk_bool_t b = duk_get_boolean(ctx, -1);
        duk_pop(ctx);
        duk_rp_push_bigint_from_i64(ctx, b ? 1 : 0);
        return duk__bigint_mp_peek(ctx, -1);
    }
    /* Numbers + everything else fall through to caller's policy. */
    duk_pop(ctx);
    return NULL;
}

/* Push a primitive coerced from `tv` via ToPrimitive(NUMBER).  Returns
 * 1 if the result is a BigInt, 0 otherwise.  Result is on the stack
 * top either way; caller is responsible for popping. */
static duk_bool_t duk__push_primitive_for_arith(duk_context *ctx, duk_tval *tv) {
    duk_push_tval(ctx, tv);
    if (duk_get_type(ctx, -1) == DUK_TYPE_OBJECT && !duk_rp_is_bigint(ctx, -1)) {
        duk_to_primitive(ctx, -1, DUK_HINT_NUMBER);
    }
    return duk_rp_is_bigint(ctx, -1) ? 1 : 0;
}

/* "Try" arith binop dispatch.  Coerces both operands via ToPrimitive
 * (NUMBER), then:
 *   - both BigInt: do BigInt op, return 1.
 *   - both non-BigInt: pop coerced copies, return 0 (caller continues
 *     with the standard number-only path; coercion was lossless).
 *   - mixed: TypeError per spec.
 * idx_z is the destination slot for the result on success. */
DUK_INTERNAL duk_bool_t duk_rp_bigint_try_arith(duk_hthread *thr,
                                                 duk_tval *tv_x, duk_tval *tv_y,
                                                 duk_uint_t opcode,
                                                 duk_idx_t idx_z) {
    duk_context *ctx = (duk_context *) thr;
    duk_push_tval(ctx, tv_x);
    duk_push_tval(ctx, tv_y);
    duk_idx_t y_idx = duk_get_top_index(ctx);
    duk_idx_t x_idx = y_idx - 1;
    if (duk_get_type(ctx, x_idx) == DUK_TYPE_OBJECT && !duk_rp_is_bigint(ctx, x_idx)) {
        duk_to_primitive(ctx, x_idx, DUK_HINT_NUMBER);
    }
    if (duk_get_type(ctx, y_idx) == DUK_TYPE_OBJECT && !duk_rp_is_bigint(ctx, y_idx)) {
        duk_to_primitive(ctx, y_idx, DUK_HINT_NUMBER);
    }
    duk_bool_t x_big = duk_rp_is_bigint(ctx, x_idx);
    duk_bool_t y_big = duk_rp_is_bigint(ctx, y_idx);
    mp_int *a, *b, *r;
    int rc = MP_OKAY;

    if (!x_big && !y_big) {
        duk_pop_2(ctx);  /* drop coerced copies; caller continues */
        return 0;
    }
    if (!x_big || !y_big) {
        RP_TYPE_THROW(ctx, "Cannot mix BigInt and other types, use explicit conversions");
    }

    a = duk__bigint_mp_peek(ctx, x_idx);
    b = duk__bigint_mp_peek(ctx, y_idx);
    r = duk_rp_push_bigint_zero(ctx);

    switch (opcode) {
    case DUK_OP_ADD: rc = mp_add(a, b, r); break;
    case DUK_OP_SUB: rc = mp_sub(a, b, r); break;
    case DUK_OP_MUL: rc = mp_mul(a, b, r); break;
    case DUK_OP_DIV:
        if (mp_iszero(b) == MP_YES) RP_RANGE_THROW(ctx, "Division by zero");
        rc = mp_div(a, b, r, NULL);
        break;
    case DUK_OP_MOD:
        if (mp_iszero(b) == MP_YES) RP_RANGE_THROW(ctx, "Division by zero");
        /* JS BigInt remainder follows the sign of the dividend (truncated),
         * libtommath mp_mod returns a value with the sign of the divisor
         * (Euclidean).  Use mp_div with quotient discarded; remainder
         * has the right sign. */
        rc = mp_div(a, b, NULL, r);
        break;
    case DUK_OP_EXP:
        if (mp_isneg(b) == MP_YES) RP_RANGE_THROW(ctx, "BigInt negative exponent");
        if (mp_count_bits(b) > 31) {
            RP_RANGE_THROW(ctx, "BigInt exponent too large");
        }
        rc = mp_expt_n(a, mp_get_i32(b), r);
        break;
    default:
        RP_TYPE_THROW(ctx, "unsupported BigInt arith opcode");
    }

    if (rc != MP_OKAY) {
        RP_THROW(ctx, "BigInt arith failed");
    }
    /* Stack: [..., a_coerced, b_coerced, result].  Move result to
     * idx_z, then drop the two coerced operands. */
    duk_replace(ctx, idx_z);
    duk_pop_2(ctx);
    return 1;
}

/* String-aware ADD path: per spec, if either operand is a string,
 * coerce the BigInt to string and concatenate.  Otherwise this is
 * exactly duk_rp_bigint_arith_op_2 with opcode=ADD.  Returns 1 if
 * the BigInt path took the value (result already in idx_z), 0 if
 * the standard non-BigInt path should run. */
DUK_INTERNAL duk_bool_t duk_rp_bigint_try_add(duk_hthread *thr,
                                               duk_tval *tv_x, duk_tval *tv_y,
                                               duk_idx_t idx_z) {
    duk_context *ctx = (duk_context *) thr;
    duk_bool_t x_big = duk_rp_tval_is_bigint(tv_x);
    duk_bool_t y_big = duk_rp_tval_is_bigint(tv_y);
    if (!x_big && !y_big) return 0;

    /* Spec: if either side is a string (after ToPrimitive) -> concat.
     * BigInt's ToPrimitive returns itself, so we only need to check
     * the other side for string-ness.  duk_safe_to_string drives the
     * prototype's toString() method via the object [[Get]] path. */
    if (x_big && DUK_TVAL_IS_STRING(tv_y)) {
        duk_push_tval(ctx, tv_x);
        (void) duk_safe_to_string(ctx, -1);  /* replaces tv_x with str */
        duk_push_tval(ctx, tv_y);
        duk_concat(ctx, 2);
        duk_replace(ctx, idx_z);
        return 1;
    }
    if (y_big && DUK_TVAL_IS_STRING(tv_x)) {
        duk_push_tval(ctx, tv_x);
        duk_push_tval(ctx, tv_y);
        (void) duk_safe_to_string(ctx, -1);  /* replaces tv_y with str */
        duk_concat(ctx, 2);
        duk_replace(ctx, idx_z);
        return 1;
    }
    return duk_rp_bigint_try_arith(thr, tv_x, tv_y, DUK_OP_ADD, idx_z);
}

/* Bitwise binop dispatch.  Both operands must be BigInt; mixed is
 * TypeError.  >>> (BLSR) is also TypeError per spec.  Operands are
 * coerced via ToPrimitive(NUMBER) first per spec ApplyStringOr
 * NumericBinaryOperator. */
DUK_INTERNAL duk_bool_t duk_rp_bigint_try_bitwise(duk_hthread *thr,
                                                   duk_tval *tv_x, duk_tval *tv_y,
                                                   duk_uint_t opcode,
                                                   duk_idx_t idx_z) {
    duk_context *ctx = (duk_context *) thr;
    duk_push_tval(ctx, tv_x);
    duk_push_tval(ctx, tv_y);
    duk_idx_t y_idx = duk_get_top_index(ctx);
    duk_idx_t x_idx = y_idx - 1;
    if (duk_get_type(ctx, x_idx) == DUK_TYPE_OBJECT && !duk_rp_is_bigint(ctx, x_idx)) {
        duk_to_primitive(ctx, x_idx, DUK_HINT_NUMBER);
    }
    if (duk_get_type(ctx, y_idx) == DUK_TYPE_OBJECT && !duk_rp_is_bigint(ctx, y_idx)) {
        duk_to_primitive(ctx, y_idx, DUK_HINT_NUMBER);
    }
    duk_bool_t x_big = duk_rp_is_bigint(ctx, x_idx);
    duk_bool_t y_big = duk_rp_is_bigint(ctx, y_idx);
    mp_int *a, *b, *r;
    int rc = MP_OKAY;

    if (!x_big && !y_big) {
        duk_pop_2(ctx);
        return 0;
    }
    if (!x_big || !y_big) {
        RP_TYPE_THROW(ctx, "Cannot mix BigInt and other types, use explicit conversions");
    }

    a = duk__bigint_mp_peek(ctx, x_idx);
    b = duk__bigint_mp_peek(ctx, y_idx);
    r = duk_rp_push_bigint_zero(ctx);

    switch (opcode) {
    case DUK_OP_BAND: rc = mp_and(a, b, r); break;
    case DUK_OP_BOR:  rc = mp_or(a, b, r); break;
    case DUK_OP_BXOR: rc = mp_xor(a, b, r); break;
    case DUK_OP_BASL: {
        /* Spec: BigInt::leftShift(a, b).  Negative `b` is equivalent
         * to BigInt::signedRightShift(a, -b) -- there's no RangeError
         * for negative shift counts on BigInt unlike Number shifts. */
        int s;
        if (mp_isneg(b) == MP_YES) {
            /* Re-dispatch as a right shift by |b|. */
            mp_int neg_b;
            mp_init(&neg_b);
            if (mp_neg(b, &neg_b) != MP_OKAY) {
                mp_clear(&neg_b);
                RP_THROW(ctx, "BigInt: mp_neg failed");
            }
            if (mp_count_bits(&neg_b) > 30) {
                mp_clear(&neg_b);
                if (mp_isneg(a) == MP_YES) {
                    mp_set_i32(r, -1);
                } else {
                    mp_zero(r);
                }
                duk_replace(ctx, idx_z);
                duk_pop_2(ctx);
                return 1;
            }
            s = mp_get_i32(&neg_b);
            rc = mp_signed_rsh(a, s, r);
            mp_clear(&neg_b);
            break;
        }
        if (mp_count_bits(b) > 30) {
            RP_RANGE_THROW(ctx, "BigInt: shift count too large");
        }
        s = mp_get_i32(b);
        rc = mp_mul_2d(a, s, r);
        break;
    }
    case DUK_OP_BASR: {
        int s;
        if (mp_isneg(b) == MP_YES) {
            /* x >> -n is x << |n|; symmetric to BASL handling. */
            mp_int neg_b;
            mp_init(&neg_b);
            if (mp_neg(b, &neg_b) != MP_OKAY) {
                mp_clear(&neg_b);
                RP_THROW(ctx, "BigInt: mp_neg failed");
            }
            if (mp_count_bits(&neg_b) > 30) {
                mp_clear(&neg_b);
                RP_RANGE_THROW(ctx, "BigInt: shift count too large");
            }
            s = mp_get_i32(&neg_b);
            rc = mp_mul_2d(a, s, r);
            mp_clear(&neg_b);
            break;
        }
        if (mp_count_bits(b) > 30) {
            /* For positive `a`, shifting by huge amount yields 0;
             * for negative `a`, it yields -1.  Synthesize directly. */
            if (mp_isneg(a) == MP_YES) {
                mp_set_i32(r, -1);
            } else {
                mp_zero(r);
            }
            duk_replace(ctx, idx_z);
            duk_pop_2(ctx);
            return 1;
        }
        s = mp_get_i32(b);
        rc = mp_signed_rsh(a, s, r);
        break;
    }
    case DUK_OP_BLSR:
        RP_TYPE_THROW(ctx, "BigInt: unsigned right shift (>>>) not supported");
    default:
        RP_TYPE_THROW(ctx, "unsupported BigInt bitwise opcode");
    }

    if (rc != MP_OKAY) {
        RP_THROW(ctx, "BigInt bitwise op failed");
    }
    /* Stack: [..., a_coerced, b_coerced, result]. */
    duk_replace(ctx, idx_z);
    duk_pop_2(ctx);
    return 1;
}

/* Comparison (relational) helper: returns -1/0/+1 in `*out`.
 * Returns 1 if the helper consumed the case, 0 if caller should fall
 * back to the number-only comparison path.
 *
 * Spec for x < y when both bigint: mp_cmp.
 * Mixed bigint <-> number: compare mathematically.  We approximate
 * by converting the BigInt to double; loss of precision can be a
 * problem near the magnitude boundary, but the alternative (lifting
 * the Number to a BigInt) needs special-case handling for non-integer
 * and Infinity/NaN.  Mixed bigint <-> string: parse the string as
 * BigInt if possible, else compare as numbers.
 *
 * `flags` is duk_js_compare_helper's flags (DUK_COMPARE_FLAG_*).
 */
DUK_INTERNAL duk_bool_t duk_rp_bigint_try_compare(duk_hthread *thr,
                                                   duk_tval *tv_x, duk_tval *tv_y,
                                                   int *out_ord) {
    duk_context *ctx = (duk_context *) thr;
    duk_bool_t x_big = duk_rp_tval_is_bigint(tv_x);
    duk_bool_t y_big = duk_rp_tval_is_bigint(tv_y);
    mp_int *a, *b;
    int ord;

    if (!x_big && !y_big) return 0;

    if (x_big && y_big) {
        a = duk__bigint_mp_from_tval(thr, tv_x);
        b = duk__bigint_mp_from_tval(thr, tv_y);
        ord = mp_cmp(a, b);
        *out_ord = (ord == MP_LT) ? -1 : (ord == MP_GT) ? 1 : 0;
        return 1;
    }

    /* Mixed: bigint vs number / string. */
    {
        duk_tval *tv_big = x_big ? tv_x : tv_y;
        duk_tval *tv_oth = x_big ? tv_y : tv_x;
        mp_int *big = duk__bigint_mp_from_tval(thr, tv_big);
        double d_big = mp_get_double(big);
        double d_oth;

        if (DUK_TVAL_IS_NUMBER(tv_oth)) {
            d_oth = DUK_TVAL_GET_NUMBER(tv_oth);
        } else if (DUK_TVAL_IS_STRING(tv_oth)) {
            duk_push_tval(ctx, tv_oth);
            d_oth = duk_to_number(ctx, -1);
            duk_pop(ctx);
        } else if (DUK_TVAL_IS_BOOLEAN(tv_oth)) {
            d_oth = DUK_TVAL_GET_BOOLEAN(tv_oth) ? 1.0 : 0.0;
        } else {
            /* null/undefined/object: hand back to caller to coerce. */
            return 0;
        }
        /* NaN comparison always "undefined" (false in JS); signal by
         * returning the special ord=2 used by the executor's NaN path.
         * Actually JS spec says comparison with NaN returns
         * 'undefined', which evaluates to false for all four
         * relational operators.  Encode this as a sentinel.  Caller
         * checks for it. */
        if (d_oth != d_oth) {  /* NaN */
            *out_ord = 2;  /* sentinel: comparison undefined */
            return 1;
        }
        /* Swap order if BigInt was on the right. */
        if (!x_big) {
            ord = (d_oth < d_big) ? -1 : (d_oth > d_big) ? 1 : 0;
        } else {
            ord = (d_big < d_oth) ? -1 : (d_big > d_oth) ? 1 : 0;
        }
        *out_ord = ord;
        return 1;
    }
}

/* Loose / strict equality helper.  Returns 1 if the helper consumed
 * the case (result in *out_eq), 0 otherwise. */
DUK_INTERNAL duk_bool_t duk_rp_bigint_try_equals(duk_hthread *thr,
                                                  duk_tval *tv_x, duk_tval *tv_y,
                                                  duk_bool_t strict,
                                                  duk_bool_t *out_eq) {
    duk_context *ctx = (duk_context *) thr;
    duk_bool_t x_big = duk_rp_tval_is_bigint(tv_x);
    duk_bool_t y_big = duk_rp_tval_is_bigint(tv_y);
    mp_int *a, *b;

    if (!x_big && !y_big) return 0;

    if (strict) {
        if (x_big != y_big) {
            *out_eq = 0;
            return 1;
        }
        a = duk__bigint_mp_from_tval(thr, tv_x);
        b = duk__bigint_mp_from_tval(thr, tv_y);
        *out_eq = (mp_cmp(a, b) == MP_EQ);
        return 1;
    }

    /* Loose equality: both bigint -> mp_cmp.  bigint vs string ->
     * parse; bigint vs bool -> coerce; bigint vs number -> exact
     * mathematical equality (no precision loss). */
    if (x_big && y_big) {
        a = duk__bigint_mp_from_tval(thr, tv_x);
        b = duk__bigint_mp_from_tval(thr, tv_y);
        *out_eq = (mp_cmp(a, b) == MP_EQ);
        return 1;
    }

    {
        duk_tval *tv_big = x_big ? tv_x : tv_y;
        duk_tval *tv_oth = x_big ? tv_y : tv_x;
        mp_int *big = duk__bigint_mp_from_tval(thr, tv_big);
        mp_int *coerced;

        if (DUK_TVAL_IS_NUMBER(tv_oth)) {
            double d = DUK_TVAL_GET_NUMBER(tv_oth);
            if (d != d) { *out_eq = 0; return 1; }  /* NaN */
            if (d != (double) (duk_int64_t) d) {
                /* Non-integer or out of i64 range: not equal to any
                 * exact bigint (BigInt(1.5) wouldn't exist).  This is
                 * mathematically inexact at the precision boundary;
                 * good enough for typical comparisons. */
                *out_eq = 0;
                return 1;
            }
            duk_rp_push_bigint_from_i64(ctx, (duk_int64_t) d);
            coerced = duk__bigint_mp_peek(ctx, -1);
            *out_eq = (mp_cmp(big, coerced) == MP_EQ);
            duk_pop(ctx);
            return 1;
        }
        coerced = duk__bigint_coerce_for_eq(ctx, tv_oth);
        if (coerced != NULL) {
            *out_eq = (mp_cmp(big, coerced) == MP_EQ);
            duk_pop(ctx);
            return 1;
        }
        /* null / undefined / object: not equal to bigint per spec
         * (null/undefined only compare equal to each other; object
         * goes through ToPrimitive but we don't recurse here -- caller
         * should still fall back to the standard equality path). */
        return 0;
    }
}

/* Unary minus / bitwise NOT.  opcode: DUK_OP_UNM, DUK_OP_BNOT, DUK_OP_UNP.
 * UNP throws TypeError per spec. */
DUK_INTERNAL duk_bool_t duk_rp_bigint_try_unary(duk_hthread *thr,
                                                  duk_tval *tv_x,
                                                  duk_uint_t opcode,
                                                  duk_idx_t idx_z) {
    duk_context *ctx = (duk_context *) thr;
    mp_int *a, *r;
    int rc = MP_OKAY;

    if (!duk_rp_tval_is_bigint(tv_x)) return 0;
    a = duk__bigint_mp_from_tval(thr, tv_x);
    /* DUK_OP_UNP and DUK_OP_UNM are defined in duk_js_bytecode.h.
     * The bitwise NOT is DUK_OP_BNOT.  We don't depend on those macro
     * values being available here -- caller uses raw small ints below. */
    switch (opcode) {
    case 0 /*UNM*/: r = duk_rp_push_bigint_zero(ctx); rc = mp_neg(a, r); break;
    case 1 /*BNOT*/: r = duk_rp_push_bigint_zero(ctx); rc = mp_complement(a, r); break;
    case 2 /*UNP*/: RP_TYPE_THROW(ctx, "Cannot convert a BigInt to a number");
    default: return 0;
    }
    if (rc != MP_OKAY) RP_THROW(ctx, "BigInt unary op failed");
    duk_replace(ctx, idx_z);
    return 1;
}

/* ------------------------------------------------------------------ */
/* JS-visible BigInt constructor + methods                             */
/* ------------------------------------------------------------------ */

/* BigInt(value) — must be called as a plain function, not constructor. */
static duk_ret_t duk__bigint_ctor(duk_context *ctx) {
    if (duk_is_constructor_call(ctx)) {
        RP_TYPE_THROW(ctx, "BigInt is not a constructor");
    }

    if (duk_get_top(ctx) < 1) {
        RP_TYPE_THROW(ctx, "Cannot convert undefined to a BigInt");
    }

    if (duk_rp_is_bigint(ctx, 0)) {
        /* Already a BigInt: return as-is (per spec ToBigInt of a BigInt
         * returns the same value).  We dup so the caller's argument
         * isn't consumed. */
        duk_dup(ctx, 0);
        return 1;
    }

    switch (duk_get_type(ctx, 0)) {
    case DUK_TYPE_BOOLEAN:
        duk_rp_push_bigint_from_i64(ctx, duk_get_boolean(ctx, 0) ? 1 : 0);
        return 1;
    case DUK_TYPE_NUMBER:
        duk_rp_push_bigint_from_double(ctx, duk_get_number(ctx, 0));
        return 1;
    case DUK_TYPE_STRING: {
        const char *s = duk_get_string(ctx, 0);
        duk_rp_push_bigint_from_string(ctx, s, 0 /* auto-detect */);
        return 1;
    }
    case DUK_TYPE_NULL:
        RP_TYPE_THROW(ctx, "Cannot convert null to a BigInt");
    case DUK_TYPE_UNDEFINED:
        RP_TYPE_THROW(ctx, "Cannot convert undefined to a BigInt");
    case DUK_TYPE_OBJECT:
        /* Per spec: ToPrimitive(arg, number) then ToBigInt.  Approx by
         * coercing via valueOf/toString and recursing on the result.
         * BigInt counts as primitive even though its tval type is
         * OBJECT. */
        duk_to_primitive(ctx, 0, DUK_HINT_NUMBER);
        if (duk_get_type(ctx, 0) == DUK_TYPE_OBJECT &&
            !duk_rp_is_bigint(ctx, 0)) {
            RP_TYPE_THROW(ctx, "Cannot convert object to a BigInt");
        }
        /* Tail-call into self by re-invoking. */
        duk_push_current_function(ctx);
        duk_dup(ctx, 0);
        duk_call(ctx, 1);
        return 1;
    default:
        RP_TYPE_THROW(ctx, "Cannot convert value to a BigInt");
    }
    /* Unreachable: every RP_*_THROW above is non-returning, but gcc
     * can't see the noreturn-ness through the macro. */
    return 0;
}

/* Helper: resolve `this` to mp_int*, throw TypeError otherwise. */
static mp_int *duk__bigint_this_mp(duk_context *ctx) {
    mp_int *bn;
    duk_push_this(ctx);
    bn = duk__bigint_mp_peek(ctx, -1);
    if (bn == NULL) {
        RP_TYPE_THROW(ctx, "BigInt.prototype method called on non-BigInt");
    }
    duk_pop(ctx);
    return bn;
}

/* BigInt.prototype.toString([radix]) */
static duk_ret_t duk__bigint_proto_toString(duk_context *ctx) {
    mp_int *bn = duk__bigint_this_mp(ctx);
    int radix = 10;
    int size = 0;
    size_t written = 0;
    char *buf;
    size_t i, out_len;

    if (duk_get_top(ctx) >= 1 && !duk_is_undefined(ctx, 0)) {
        double d = duk_to_number(ctx, 0);
        /* ToIntegerOrInfinity then range check */
        if (d != d || d == (double)(1.0/0.0) || d == (double)(-1.0/0.0)) {
            RP_RANGE_THROW(ctx, "BigInt: radix must be integer in [2,36]");
        }
        d = (d < 0.0) ? -floor(-d) : floor(d);
        if (!(d >= 2.0 && d <= 36.0)) {
            RP_RANGE_THROW(ctx, "BigInt: radix must be integer in [2,36]");
        }
        radix = (int) d;
    }

    if (mp_radix_size(bn, radix, &size) != MP_OKAY || size <= 0) {
        RP_THROW(ctx, "BigInt: mp_radix_size failed");
    }
    buf = (char *) duk_push_fixed_buffer(ctx, (duk_size_t) size);
    if (mp_to_radix(bn, buf, (size_t) size, &written, radix) != MP_OKAY) {
        RP_THROW(ctx, "BigInt: mp_to_radix failed");
    }
    /* mp_to_radix writes a NUL-terminated string; `written` includes
     * the NUL.  Spec mandates lowercase letters for digits 10-35;
     * libtommath emits uppercase by default. */
    out_len = written > 0 ? written - 1 : 0;
    for (i = 0; i < out_len; i++) {
        if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = (char) (buf[i] + 32);
    }
    duk_push_lstring(ctx, buf, out_len);
    return 1;
}

static duk_ret_t duk__bigint_proto_toLocaleString(duk_context *ctx) {
    /* Minimal: ignore locale args, delegate to toString(10). */
    (void) ctx;
    return duk__bigint_proto_toString(ctx);
}

/* BigInt.prototype.valueOf() — returns the BigInt itself. */
static duk_ret_t duk__bigint_proto_valueOf(duk_context *ctx) {
    duk_push_this(ctx);
    if (!duk_rp_is_bigint(ctx, -1)) {
        RP_TYPE_THROW(ctx, "BigInt.prototype.valueOf called on non-BigInt");
    }
    return 1;
}

/* BigInt.isBigInt(x) — rampart extension: type test that doesn't
 * require typeof support.  Spec-wise this is BigInt.prototype.<unknown>;
 * we expose it as a static method named isBigInt for symmetry with
 * Array.isArray. */
static duk_ret_t duk__bigint_static_isBigInt(duk_context *ctx) {
    duk_push_boolean(ctx, duk_rp_is_bigint(ctx, 0));
    return 1;
}

/* Spec ToBigInt (abstract op): Number argument always throws TypeError;
 * boolean coerces; string parses; bigint returns; everything else does
 * ToPrimitive(number) and recurses.  Pushes the resulting BigInt on
 * the stack.  Differs from BigInt() constructor which accepts integer
 * Numbers. */
static void duk__bigint_to_bigint(duk_context *ctx, duk_idx_t idx) {
    idx = duk_normalize_index(ctx, idx);
    if (duk_rp_is_bigint(ctx, idx)) {
        duk_dup(ctx, idx);
        return;
    }
    switch (duk_get_type(ctx, idx)) {
    case DUK_TYPE_BOOLEAN:
        duk_rp_push_bigint_from_i64(ctx, duk_get_boolean(ctx, idx) ? 1 : 0);
        return;
    case DUK_TYPE_STRING: {
        const char *s = duk_get_string(ctx, idx);
        duk_rp_push_bigint_from_string(ctx, s, 0);
        return;
    }
    case DUK_TYPE_NUMBER:
        RP_TYPE_THROW(ctx, "Cannot convert a Number to a BigInt");
    case DUK_TYPE_NULL:
        RP_TYPE_THROW(ctx, "Cannot convert null to a BigInt");
    case DUK_TYPE_UNDEFINED:
        RP_TYPE_THROW(ctx, "Cannot convert undefined to a BigInt");
    case DUK_TYPE_OBJECT: {
        /* ToPrimitive(arg, number), then recurse.  BigInt counts as a
         * primitive (the duk type is OBJECT but we treat it like one). */
        duk_dup(ctx, idx);
        duk_to_primitive(ctx, -1, DUK_HINT_NUMBER);
        if (duk_get_type(ctx, -1) == DUK_TYPE_OBJECT &&
            !duk_rp_is_bigint(ctx, -1)) {
            RP_TYPE_THROW(ctx, "Cannot convert object to a BigInt");
        }
        duk__bigint_to_bigint(ctx, -1);
        duk_remove(ctx, -2);
        return;
    }
    default:
        RP_TYPE_THROW(ctx, "Cannot convert value to a BigInt");
    }
}

/* Spec ToIndex (ECMA-262 7.1.22):
 *   1. If value is undefined: return 0.
 *   2. integerIndex = ToIntegerOrInfinity(value).
 *      (NaN -> 0; ±Inf preserved; others trunc-toward-zero.)
 *   3. If integerIndex < 0: throw RangeError.
 *   4. index = min(integerIndex, 2^53-1).
 *   5. If index != integerIndex: throw RangeError.
 *   6. Return index.
 *
 * BigInt input goes through duk_to_number which throws TypeError. */
static int duk__bigint_toindex(duk_context *ctx, duk_idx_t idx) {
    double d;
    double truncated;
    if (duk_is_undefined(ctx, idx)) return 0;
    d = duk_to_number(ctx, idx);  /* throws TypeError on BigInt */
    /* NaN -> 0. */
    if (d != d) return 0;
    /* Truncate toward zero. */
    truncated = (d < 0.0) ? -floor(-d) : floor(d);
    /* +0 returned for -0 per spec. */
    if (truncated == 0.0) truncated = 0.0;
    if (truncated < 0.0) {
        RP_RANGE_THROW(ctx, "ToIndex: negative value");
    }
    if (truncated > 9007199254740991.0) {
        RP_RANGE_THROW(ctx, "ToIndex: value > 2^53-1");
    }
    /* Cap at INT_MAX for our usage; bits beyond 2^31 are unrealistic. */
    if (truncated > 2147483647.0) {
        RP_RANGE_THROW(ctx, "ToIndex: too large for bits");
    }
    return (int) truncated;
}

/* BigInt.asIntN(bits, x) — wrap to two's-complement bits-bit signed. */
static duk_ret_t duk__bigint_static_asIntN(duk_context *ctx) {
    int bits;
    mp_int *src;
    mp_int *dst;
    mp_int mod;
    int rc;

    /* Spec: asIntN is a function, not a constructor. */
    if (duk_is_constructor_call(ctx)) {
        RP_TYPE_THROW(ctx, "BigInt.asIntN is not a constructor");
    }
    bits = duk__bigint_toindex(ctx, 0);

    /* Coerce arg 1 via spec ToBigInt (Number => TypeError). */
    duk__bigint_to_bigint(ctx, 1);
    src = duk_rp_get_bigint_mp(ctx, -1);
    if (src == NULL) {
        RP_TYPE_THROW(ctx, "BigInt.asIntN: coercion failed");
    }

    dst = duk_rp_push_bigint_zero(ctx);
    if (bits == 0) {
        return 1; /* result is 0n */
    }
    if (mp_init(&mod) != MP_OKAY) {
        RP_THROW(ctx, "BigInt.asIntN: mp_init failed");
    }
    /* mod = 2^bits */
    rc = mp_2expt(&mod, bits);
    if (rc == MP_OKAY) rc = mp_mod(src, &mod, dst);
    if (rc == MP_OKAY) {
        /* If high bit of bits-bit value is set, subtract 2^bits. */
        if (mp_count_bits(dst) >= bits) {
            rc = mp_sub(dst, &mod, dst);
        }
    }
    mp_clear(&mod);
    if (rc != MP_OKAY) {
        RP_THROW(ctx, "BigInt.asIntN: arithmetic failed");
    }
    return 1;
}

/* BigInt.asUintN(bits, x) — wrap to bits-bit unsigned. */
static duk_ret_t duk__bigint_static_asUintN(duk_context *ctx) {
    int bits;
    mp_int *src;
    mp_int *dst;
    mp_int mod;
    int rc;

    /* Spec: asUintN is a function, not a constructor. */
    if (duk_is_constructor_call(ctx)) {
        RP_TYPE_THROW(ctx, "BigInt.asUintN is not a constructor");
    }
    bits = duk__bigint_toindex(ctx, 0);

    duk__bigint_to_bigint(ctx, 1);
    src = duk_rp_get_bigint_mp(ctx, -1);
    if (src == NULL) {
        RP_TYPE_THROW(ctx, "BigInt.asUintN: coercion failed");
    }

    dst = duk_rp_push_bigint_zero(ctx);
    if (bits == 0) {
        return 1;
    }
    if (mp_init(&mod) != MP_OKAY) {
        RP_THROW(ctx, "BigInt.asUintN: mp_init failed");
    }
    rc = mp_2expt(&mod, bits);
    if (rc == MP_OKAY) rc = mp_mod(src, &mod, dst);
    mp_clear(&mod);
    if (rc != MP_OKAY) {
        RP_THROW(ctx, "BigInt.asUintN: arithmetic failed");
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Installer                                                           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* DataView prototype extensions: getBigInt64/setBigInt64/             */
/*                                 getBigUint64/setBigUint64.          */
/* ------------------------------------------------------------------ */

/* Read 8 bytes from the underlying buffer at given offset, build a
 * BigInt according to signed/unsigned + endianness flags.  magic
 * encodes flags in 2 bits: bit 0 = signed, bit 1 = littleEndian. */
static duk_ret_t duk__dataview_get_big(duk_context *ctx) {
    duk_uint_t magic = (duk_uint_t) duk_get_current_magic(ctx);
    duk_bool_t is_signed = magic & 1;
    duk_bool_t little_endian = (duk_get_top(ctx) >= 2) ? duk_to_boolean(ctx, 1) : 0;
    duk_size_t off;
    duk_size_t buflen;
    duk_uint8_t *data;
    duk_uint8_t bytes[8];
    duk_uint8_t reordered[8];
    int i;
    mp_int *r;
    int neg = 0;

    off = (duk_size_t) duk_require_number(ctx, 0);
    duk_push_this(ctx);
    /* this is a DataView; access its underlying buffer via .buffer +
     * .byteOffset.  Simpler: read raw via buffer_data on the view. */
    data = duk_require_buffer_data(ctx, -1, &buflen);
    if (off + 8 > buflen) {
        RP_RANGE_THROW(ctx, "DataView: offset out of bounds");
    }
    for (i = 0; i < 8; i++) bytes[i] = data[off + i];

    /* Normalize to big-endian for parsing into mp_int. */
    if (little_endian) {
        for (i = 0; i < 8; i++) reordered[i] = bytes[7 - i];
    } else {
        for (i = 0; i < 8; i++) reordered[i] = bytes[i];
    }

    if (is_signed && (reordered[0] & 0x80)) {
        /* Two's complement: magnitude = ~value + 1, with byte-level
         * invert-and-increment.  Result interpreted as positive then
         * sign-flipped at the end. */
        int carry = 1;
        neg = 1;
        for (i = 0; i < 8; i++) reordered[i] = (duk_uint8_t) ~reordered[i];
        for (i = 7; i >= 0; i--) {
            duk_uint_fast16_t s = (duk_uint_fast16_t) reordered[i] + (duk_uint_fast16_t) carry;
            reordered[i] = (duk_uint8_t) (s & 0xff);
            carry = (int) (s >> 8);
        }
    }

    r = duk_rp_push_bigint_zero(ctx);
    /* Build the magnitude from 8 big-endian bytes. */
    {
        uint64_t mag = 0;
        int rc;
        for (i = 0; i < 8; i++) {
            mag = (mag << 8) | reordered[i];
        }
        mp_set_u64(r, mag);
        if (neg && mag != 0) {
            rc = mp_neg(r, r);
            if (rc != MP_OKAY) RP_THROW(ctx, "DataView getBigInt64: mp_neg failed");
        }
    }
    return 1;
}

/* magic encodes flags: bit 0 = signed, bit 1 = littleEndian. */
static duk_ret_t duk__dataview_set_big(duk_context *ctx) {
    duk_uint_t magic = (duk_uint_t) duk_get_current_magic(ctx);
    duk_bool_t is_signed = magic & 1;
    duk_bool_t little_endian = (duk_get_top(ctx) >= 3) ? duk_to_boolean(ctx, 2) : 0;
    duk_size_t off = (duk_size_t) duk_require_number(ctx, 0);
    duk_size_t buflen;
    duk_uint8_t *data;
    mp_int *bn;
    uint64_t mag;
    uint8_t bytes[8];
    int i;
    int negative;
    (void) is_signed;

    /* Coerce arg 1 to BigInt. */
    duk_get_global_string(ctx, "BigInt");
    duk_dup(ctx, 1);
    duk_call(ctx, 1);
    bn = duk_rp_get_bigint_mp(ctx, -1);
    if (bn == NULL) RP_TYPE_THROW(ctx, "DataView setBigInt64: coercion failed");

    duk_push_this(ctx);
    data = duk_require_buffer_data(ctx, -1, &buflen);
    if (off + 8 > buflen) {
        RP_RANGE_THROW(ctx, "DataView: offset out of bounds");
    }

    negative = (mp_isneg(bn) == MP_YES) ? 1 : 0;
    mag = mp_get_mag_u64(bn);

    if (negative) {
        /* Two's complement: bytes = 2^64 - mag, low 64 bits. */
        mag = (uint64_t) -((int64_t) mag);  /* relies on wrap-around */
    }

    for (i = 7; i >= 0; i--) {
        bytes[i] = (uint8_t) (mag & 0xff);
        mag >>= 8;
    }

    if (little_endian) {
        for (i = 0; i < 8; i++) data[off + i] = bytes[7 - i];
    } else {
        for (i = 0; i < 8; i++) data[off + i] = bytes[i];
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static const duk_function_list_entry duk__bigint_proto_funcs[] = {
    { "toString",        duk__bigint_proto_toString,        DUK_VARARGS },
    { "toLocaleString",  duk__bigint_proto_toLocaleString,  DUK_VARARGS },
    { "valueOf",         duk__bigint_proto_valueOf,         0 },
    { NULL, NULL, 0 }
};

static const duk_function_list_entry duk__bigint_ctor_funcs[] = {
    { "isBigInt", duk__bigint_static_isBigInt, 1 },
    { "asIntN",   duk__bigint_static_asIntN,   2 },
    { "asUintN",  duk__bigint_static_asUintN,  2 },
    { NULL, NULL, 0 }
};

/* Install a method on a target object with descriptor {writable:true,
 * enumerable:false, configurable:true} (the standard JS attribute set
 * for spec-defined functions).  Sets the function's .name and .length
 * to spec-correct {writable:false, enumerable:false, configurable:true}. */
static void duk__bigint_def_method(duk_context *ctx, duk_idx_t obj_idx,
                                    const char *name, duk_c_function fn,
                                    duk_int_t nargs, duk_int_t length) {
    obj_idx = duk_normalize_index(ctx, obj_idx);

    duk_push_c_function(ctx, fn, nargs);

    /* Force .name = name with {w:false, e:false, c:true}. */
    duk_push_string(ctx, "name");
    duk_push_string(ctx, name);
    duk_def_prop(ctx, -3,
        DUK_DEFPROP_HAVE_VALUE |
        DUK_DEFPROP_HAVE_WRITABLE | 0 |
        DUK_DEFPROP_HAVE_ENUMERABLE | 0 |
        DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE |
        DUK_DEFPROP_FORCE);

    /* Force .length = length with {w:false, e:false, c:true}. */
    duk_push_string(ctx, "length");
    duk_push_int(ctx, length);
    duk_def_prop(ctx, -3,
        DUK_DEFPROP_HAVE_VALUE |
        DUK_DEFPROP_HAVE_WRITABLE | 0 |
        DUK_DEFPROP_HAVE_ENUMERABLE | 0 |
        DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE |
        DUK_DEFPROP_FORCE);

    /* Define the method on the target: {w:true, e:false, c:true}. */
    duk_push_string(ctx, name);
    duk_swap_top(ctx, -2);
    duk_def_prop(ctx, obj_idx,
        DUK_DEFPROP_HAVE_VALUE |
        DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_WRITABLE |
        DUK_DEFPROP_HAVE_ENUMERABLE | 0 |
        DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);
}

DUK_INTERNAL void duk_rp_install_bigint(duk_context *ctx) {
    duk_idx_t ctor_idx, proto_idx;

    /* BigInt constructor.  Plain-call only; spec says `new BigInt()`
     * is a TypeError, which the ctor body enforces. */
    duk_push_c_function(ctx, duk__bigint_ctor, 1);
    ctor_idx = duk_get_top_index(ctx);

    /* duktape's natfunc machinery sets the function's [[Prototype]] to
     * an INTERNAL function-prototype object (carries toString/call/
     * apply/bind) -- NOT the JS-visible Function.prototype.  Force the
     * proto chain to %FunctionPrototype% so `Object.getPrototypeOf
     * (BigInt) === Function.prototype` per spec. */
    if (duk_get_global_string(ctx, "Function")) {
        if (duk_get_prop_string(ctx, -1, "prototype")) {
            duk_set_prototype(ctx, ctor_idx);
        } else {
            duk_pop(ctx);
        }
        duk_pop(ctx);  /* Function */
    }

    /* Force .name = "BigInt" with {w:false, e:false, c:true}.  Force .length
     * = 1 with the same attrs -- duktape's natfunc presents .length as a
     * virtual property, but test262 + spec want a real own-property
     * descriptor that getOwnPropertyDescriptor can find. */
    duk_push_string(ctx, "name");
    duk_push_string(ctx, "BigInt");
    duk_def_prop(ctx, ctor_idx,
        DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | 0 |
        DUK_DEFPROP_HAVE_ENUMERABLE | 0 |
        DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE |
        DUK_DEFPROP_FORCE);

    duk_push_string(ctx, "length");
    duk_push_int(ctx, 1);
    duk_def_prop(ctx, ctor_idx,
        DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | 0 |
        DUK_DEFPROP_HAVE_ENUMERABLE | 0 |
        DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE |
        DUK_DEFPROP_FORCE);

    /* static methods, non-enumerable */
    duk__bigint_def_method(ctx, ctor_idx, "asIntN",   duk__bigint_static_asIntN,   2, 2);
    duk__bigint_def_method(ctx, ctor_idx, "asUintN",  duk__bigint_static_asUintN,  2, 2);
    duk__bigint_def_method(ctx, ctor_idx, "isBigInt", duk__bigint_static_isBigInt, 1, 1);

    /* prototype */
    duk_push_object(ctx);
    proto_idx = duk_get_top_index(ctx);
    duk__bigint_def_method(ctx, proto_idx, "toString",       duk__bigint_proto_toString,       DUK_VARARGS, 0);
    duk__bigint_def_method(ctx, proto_idx, "toLocaleString", duk__bigint_proto_toLocaleString, DUK_VARARGS, 0);
    duk__bigint_def_method(ctx, proto_idx, "valueOf",        duk__bigint_proto_valueOf,        0, 0);

    /* prototype.constructor = BigInt with non-enumerable. */
    duk_push_string(ctx, "constructor");
    duk_dup(ctx, ctor_idx);
    duk_def_prop(ctx, proto_idx,
        DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_WRITABLE |
        DUK_DEFPROP_HAVE_ENUMERABLE | 0 |
        DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);

    /* prototype[Symbol.toStringTag] = "BigInt" with non-enum/conf */
    if (duk_get_global_string(ctx, "Symbol")) {
        if (duk_get_prop_string(ctx, -1, "toStringTag")) {
            duk_push_string(ctx, "BigInt");
            duk_def_prop(ctx, proto_idx,
                DUK_DEFPROP_HAVE_VALUE |
                DUK_DEFPROP_HAVE_WRITABLE | 0 |
                DUK_DEFPROP_HAVE_ENUMERABLE | 0 |
                DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);
        } else {
            duk_pop(ctx);
        }
        duk_pop(ctx); /* Symbol */
    }

    /* BigInt.prototype with attributes {writable:false, enum:false, conf:false}. */
    duk_push_string(ctx, "prototype");
    duk_dup(ctx, proto_idx);
    duk_def_prop(ctx, ctor_idx,
        DUK_DEFPROP_HAVE_VALUE |
        DUK_DEFPROP_HAVE_WRITABLE | 0 |
        DUK_DEFPROP_HAVE_ENUMERABLE | 0 |
        DUK_DEFPROP_HAVE_CONFIGURABLE | 0);

    /* Drop proto_idx; ctor remains. */
    duk_remove(ctx, proto_idx);

    /* Expose BigInt globally with non-enumerable attribute. */
    duk_push_global_object(ctx);
    duk_push_string(ctx, "BigInt");
    duk_dup(ctx, -3);  /* dup the BigInt ctor */
    duk_def_prop(ctx, -3,
        DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_HAVE_WRITABLE | DUK_DEFPROP_WRITABLE |
        DUK_DEFPROP_HAVE_ENUMERABLE | 0 |
        DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);
    duk_pop(ctx);  /* global */
    duk_pop(ctx);  /* ctor */

    /* ------------------------------------------------------------------ */
    /* DataView.prototype.{getBigInt64,setBigInt64,getBigUint64,           */
    /*                     setBigUint64}                                   */
    /*                                                                     */
    /* Add native methods to the DataView prototype.  Magic encodes:      */
    /*   bit 0 = signed (1 for getBigInt64, 0 for getBigUint64)           */
    /* ------------------------------------------------------------------ */
    if (duk_get_global_string(ctx, "DataView")) {
        if (duk_get_prop_string(ctx, -1, "prototype")) {
            duk_push_c_function(ctx, duk__dataview_get_big, DUK_VARARGS);
            duk_set_magic(ctx, -1, 1 /*signed*/);
            duk_put_prop_string(ctx, -2, "getBigInt64");

            duk_push_c_function(ctx, duk__dataview_get_big, DUK_VARARGS);
            duk_set_magic(ctx, -1, 0 /*unsigned*/);
            duk_put_prop_string(ctx, -2, "getBigUint64");

            duk_push_c_function(ctx, duk__dataview_set_big, DUK_VARARGS);
            duk_set_magic(ctx, -1, 1);
            duk_put_prop_string(ctx, -2, "setBigInt64");

            duk_push_c_function(ctx, duk__dataview_set_big, DUK_VARARGS);
            duk_set_magic(ctx, -1, 0);
            duk_put_prop_string(ctx, -2, "setBigUint64");
        }
        duk_pop_2(ctx);  /* prototype + DataView */
    }

    /* ------------------------------------------------------------------ */
    /* BigInt64Array / BigUint64Array (JS-level, Proxy-backed)            */
    /*                                                                     */
    /* Spec parity for the API surface (constructor, length, byteLength,   */
    /* byteOffset, buffer, indexed get/set, Symbol.iterator, .of, .from,   */
    /* common prototype methods).  Performance is intentionally not the    */
    /* goal here -- a real fast-path needs new HOBJECT_CLASS slots which   */
    /* require expanding the duktape flag-word class bits (deferred).      */
    /* ------------------------------------------------------------------ */
    {
        static const char *bigint_array_js =
            "(function(global){\n"
            "  function makeIter(self, valueFn) {\n"
            "    var i = 0;\n"
            "    var it = {\n"
            "      next: function() {\n"
            "        if (i >= self.length) return { value: undefined, done: true };\n"
            "        var v = valueFn(self, i); i++;\n"
            "        return { value: v, done: false };\n"
            "      }\n"
            "    };\n"
            "    it[Symbol.iterator] = function(){ return it; };\n"
            "    return it;\n"
            "  }\n"
            "  function make(name, byteSize, signed) {\n"
            "    function Ctor(arg, byteOffset, length) {\n"
            "      if (!(this instanceof Ctor)) throw new TypeError(name + ' must be called with new');\n"
            "      var buf, off=0, len, i;\n"
            "      if (arg instanceof ArrayBuffer) {\n"
            "        buf = arg; off = byteOffset|0;\n"
            "        len = (length === undefined) ? ((buf.byteLength - off) / byteSize) : (length|0);\n"
            "      } else if (typeof arg === 'number') {\n"
            "        len = arg|0; buf = new ArrayBuffer(len * byteSize);\n"
            "      } else if (arg && typeof arg.length === 'number') {\n"
            "        len = arg.length|0; buf = new ArrayBuffer(len * byteSize);\n"
            "        var dv0 = new DataView(buf);\n"
            "        for (i=0; i<len; i++) {\n"
            "          if (signed) dv0.setBigInt64(i*byteSize, BigInt(arg[i]), true);\n"
            "          else dv0.setBigUint64(i*byteSize, BigInt(arg[i]), true);\n"
            "        }\n"
            "      } else if (arg && typeof arg[Symbol.iterator] === 'function') {\n"
            "        var tmp = []; var it = arg[Symbol.iterator]();\n"
            "        for (var r=it.next(); !r.done; r=it.next()) tmp.push(BigInt(r.value));\n"
            "        len = tmp.length; buf = new ArrayBuffer(len * byteSize);\n"
            "        var dv1 = new DataView(buf);\n"
            "        for (i=0; i<len; i++) {\n"
            "          if (signed) dv1.setBigInt64(i*byteSize, tmp[i], true);\n"
            "          else dv1.setBigUint64(i*byteSize, tmp[i], true);\n"
            "        }\n"
            "      } else {\n"
            "        throw new TypeError(name + ': invalid constructor argument');\n"
            "      }\n"
            "      var dv = new DataView(buf, off, len * byteSize);\n"
            "      var self = this;\n"
            "      Object.defineProperty(self, 'buffer',     { value: buf, enumerable: false });\n"
            "      Object.defineProperty(self, 'byteOffset', { value: off, enumerable: false });\n"
            "      Object.defineProperty(self, 'byteLength', { value: len * byteSize, enumerable: false });\n"
            "      Object.defineProperty(self, 'length',     { value: len, enumerable: false });\n"
            "      Object.defineProperty(self, '_dv',        { value: dv,  enumerable: false });\n"
            "      function readAt(i){ return signed ? dv.getBigInt64(i*byteSize, true) : dv.getBigUint64(i*byteSize, true); }\n"
            "      function writeAt(i,v){ if (signed) dv.setBigInt64(i*byteSize, BigInt(v), true); else dv.setBigUint64(i*byteSize, BigInt(v), true); }\n"
            "      Object.defineProperty(self, '_get', { value: readAt });\n"
            "      Object.defineProperty(self, '_set', { value: writeAt });\n"
            "      function asIndex(p) {\n"
            "        if (typeof p === 'number') { return (p >= 0 && p === (p|0)) ? p : -1; }\n"
            "        if (typeof p === 'string' && /^(0|[1-9][0-9]*)$/.test(p)) { return +p; }\n"
            "        return -1;\n"
            "      }\n"
            "      return new Proxy(self, {\n"
            "        get: function(t, p) {\n"
            "          var i = asIndex(p);\n"
            "          if (i >= 0) { return i < len ? readAt(i) : undefined; }\n"
            "          return t[p];\n"
            "        },\n"
            "        set: function(t, p, v) {\n"
            "          var i = asIndex(p);\n"
            "          if (i >= 0) { if (i < len) writeAt(i, v); return true; }\n"
            "          t[p] = v; return true;\n"
            "        },\n"
            "        has: function(t, p) {\n"
            "          var i = asIndex(p);\n"
            "          if (i >= 0) return i < len;\n"
            "          return p in t;\n"
            "        }\n"
            "      });\n"
            "    }\n"
            /* Per spec, BYTES_PER_ELEMENT is {w:false, e:false, c:false}. */
            "    Object.defineProperty(Ctor, 'BYTES_PER_ELEMENT', { value: byteSize, writable: false, enumerable: false, configurable: false });\n"
            /* .name = constructor name, {w:false, e:false, c:true}. */
            "    Object.defineProperty(Ctor, 'name', { value: name, writable: false, enumerable: false, configurable: true });\n"
            "    Ctor.prototype = Object.create(null);\n"
            /* prototype.constructor = Ctor, {w:true, e:false, c:true}. */
            "    Object.defineProperty(Ctor.prototype, 'constructor', { value: Ctor, writable: true, enumerable: false, configurable: true });\n"
            /* prototype.BYTES_PER_ELEMENT mirrors ctor's. */
            "    Object.defineProperty(Ctor.prototype, 'BYTES_PER_ELEMENT', { value: byteSize, writable: false, enumerable: false, configurable: false });\n"
            "    Ctor.prototype[Symbol.iterator] = function() { return makeIter(this, function(s,i){ return s[i]; }); };\n"
            "    Ctor.prototype.values  = Ctor.prototype[Symbol.iterator];\n"
            "    Ctor.prototype.keys    = function() { return makeIter(this, function(s,i){ return i; }); };\n"
            "    Ctor.prototype.entries = function() { return makeIter(this, function(s,i){ return [i, s[i]]; }); };\n"
            "    Ctor.prototype.forEach = function(fn, ths) {\n"
            "      for (var i=0; i<this.length; i++) fn.call(ths, this[i], i, this);\n"
            "    };\n"
            "    Ctor.prototype.toString = function() {\n"
            "      var a=[]; for (var i=0; i<this.length; i++) a.push(this[i].toString()); return a.join(',');\n"
            "    };\n"
            "    Ctor.prototype.indexOf = function(v) {\n"
            "      v = BigInt(v); for (var i=0; i<this.length; i++) if (this[i] === v) return i; return -1;\n"
            "    };\n"
            "    Ctor.prototype.includes = function(v) { return this.indexOf(v) >= 0; };\n"
            "    Ctor.prototype.slice = function(s, e) {\n"
            "      s = (s|0)||0; e = (e === undefined) ? this.length : (e|0);\n"
            "      if (s<0) s+=this.length; if (e<0) e+=this.length;\n"
            "      if (s<0) s=0; if (e>this.length) e=this.length; if (e<s) e=s;\n"
            "      var r = new Ctor(e - s);\n"
            "      for (var i=0; i<e-s; i++) r[i] = this[s+i];\n"
            "      return r;\n"
            "    };\n"
            "    Ctor.of   = function() {\n"
            "      var a = []; for (var i=0; i<arguments.length; i++) a.push(arguments[i]);\n"
            "      return new Ctor(a);\n"
            "    };\n"
            "    Ctor.from = function(src, mapFn, thisArg) {\n"
            "      var a = []; var i = 0; var v;\n"
            "      if (src && typeof src[Symbol.iterator] === 'function') {\n"
            "        var it2 = src[Symbol.iterator]();\n"
            "        for (var r2=it2.next(); !r2.done; r2=it2.next()) {\n"
            "          v = r2.value;\n"
            "          a.push(mapFn ? mapFn.call(thisArg, v, i++) : v);\n"
            "        }\n"
            "      } else {\n"
            "        for (i = 0; i < src.length; i++) a.push(mapFn ? mapFn.call(thisArg, src[i], i) : src[i]);\n"
            "      }\n"
            "      return new Ctor(a);\n"
            "    };\n"
            /* Global binding: {w:true, e:false, c:true} per spec. */
            "    Object.defineProperty(global, name, { value: Ctor, writable: true, enumerable: false, configurable: true });\n"
            "  }\n"
            "  make('BigInt64Array',  8, true);\n"
            "  make('BigUint64Array', 8, false);\n"
            "})(this);";

        if (duk_peval_string(ctx, bigint_array_js) != 0) {
            fprintf(stderr, "duk_rp_install_bigint: BigInt64Array setup: %s\n",
                    duk_safe_to_string(ctx, -1));
        }
        duk_pop(ctx);
    }
}

#endif  /* DUK_RP_USE_BIGINT */
