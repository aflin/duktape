/*
 *  duk_rp_typedarray_extras.c
 *
 *  Spec-conformance polish for the TypedArray prototypes.  Currently
 *  installs %TypedArray%.prototype[@@toStringTag] as a proper accessor
 *  descriptor (ES2015 22.2.3.31) -- duktape ships without it, even
 *  though Object.prototype.toString already prints "[object X]" via
 *  the class-number to-stridx table.
 *
 *  Spec wants the descriptor exactly:
 *    {
 *      get: function() { return [[TypedArrayName]] of `this`, or undefined; },
 *      set: undefined,
 *      enumerable: false,
 *      configurable: true
 *    }
 *
 *  Gated by DUK_RP_USE_TYPEDARRAY_EXTRAS in util/rp_config.h.  Auto-
 *  installed by duk_rp_install_extensions() in duk_rp_extensions_init.c.
 *
 *  Future additions under the same flag may include the matching
 *  accessors on ArrayBuffer.prototype + DataView.prototype, brand
 *  checks for the buffer/byteLength/byteOffset accessors, etc.
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_TYPEDARRAY_EXTRAS)

#include "duk_rp_internal.h"

/* The %TypedArray%.prototype[@@toStringTag] getter.
 *
 * Returns the receiver's TypedArray subtype name ("Int8Array" ...
 * "Float64Array") or undefined when the receiver lacks a
 * [[TypedArrayName]] slot.  Per spec this returns UNDEFINED (does
 * NOT throw) for null / undefined / primitive / non-TypedArray
 * receivers.
 *
 * Uses duktape's existing duk_class_number_to_stridx[] table -- each
 * BUFOBJ subtype class already maps to its name stridx for the
 * Object.prototype.toString path. */
static duk_ret_t duk__rp_typedarray_tostringtag_getter(duk_context *ctx) {
    duk_hthread *thr = (duk_hthread *) ctx;
    duk_hobject *h;
    duk_small_uint_t cls;
    duk_small_uint_t stridx;

    duk_push_this(ctx);
    if (!duk_is_object(ctx, -1)) {
        duk_push_undefined(ctx);
        return 1;
    }
    h = (duk_hobject *) duk_get_heapptr(ctx, -1);
    cls = DUK_HOBJECT_GET_CLASS_NUMBER(h);
    /* TypedArray subtypes occupy a contiguous class range.  Excludes
     * ArrayBuffer, DataView, and the per-subtype prototype objects
     * (which have class OBJECT, not CLASS_*ARRAY). */
    if (cls < DUK_HOBJECT_CLASS_INT8ARRAY ||
        cls > DUK_HOBJECT_CLASS_FLOAT64ARRAY) {
        duk_push_undefined(ctx);
        return 1;
    }
    stridx = (duk_small_uint_t) DUK_HOBJECT_CLASS_NUMBER_TO_STRIDX(cls);
    duk_push_hstring_stridx(thr, stridx);
    return 1;
}

DUK_INTERNAL void duk_rp_install_typedarray_extras(duk_context *ctx) {
    /* Resolve %TypedArray%.prototype = Object.getPrototypeOf(Int8Array.prototype).
     * That intrinsic exists in duktape (bi_typedarray_prototype) but
     * isn't exposed via a global name; the prototype chain is the
     * canonical handle. */
    if (!duk_get_global_string(ctx, "Int8Array")) {
        duk_pop(ctx);
        return;
    }
    if (!duk_get_prop_string(ctx, -1, "prototype")) {
        duk_pop_2(ctx);
        return;
    }
    duk_get_prototype(ctx, -1);   /* %TypedArray%.prototype */
    duk_remove(ctx, -2);          /* Int8Array.prototype */
    duk_remove(ctx, -2);          /* Int8Array */
    /* stack top: %TypedArray%.prototype */

    /* Build the property key (the well-known Symbol.toStringTag). */
    if (!duk_get_global_string(ctx, "Symbol")) {
        duk_pop_2(ctx);
        return;
    }
    if (!duk_get_prop_string(ctx, -1, "toStringTag")) {
        duk_pop_3(ctx);
        return;
    }
    duk_remove(ctx, -2);          /* Symbol global */
    /* stack: %TypedArray%.prototype, Symbol.toStringTag */

    /* Push the getter function (0-arg native). */
    duk_push_c_function(ctx, duk__rp_typedarray_tostringtag_getter, 0);

    /* Define as accessor: {get, set: undefined, enumerable: false,
     * configurable: true}.  HAVE_SETTER without a setter on the
     * stack would error; omit the SETTER bits entirely. */
    duk_def_prop(ctx, -3,
        DUK_DEFPROP_HAVE_GETTER |
        DUK_DEFPROP_HAVE_ENUMERABLE | 0 |
        DUK_DEFPROP_HAVE_CONFIGURABLE | DUK_DEFPROP_CONFIGURABLE);

    duk_pop(ctx);  /* %TypedArray%.prototype */
}

#endif  /* DUK_RP_USE_TYPEDARRAY_EXTRAS */
