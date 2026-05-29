/* duk_rp_map_set.c -- Map and Set implementations for Duktape
 *
 * Copyright (C) 2026 Aaron Flin - All Rights Reserved
 * MIT license -- https://opensource.org/licenses/MIT
 *
 * Rampart contribution (post duktape 2.7.0).  Gated by
 * DUK_RP_USE_MAP_SET in util/rp_config.h.  Auto-installed by
 * duk_rp_install_extensions() in duk_rp_extensions_init.c.
 *
 * Origin: rampart's src/duktape/globals/rampart-map.c.
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_MAP_SET)

#include "duk_rp_internal.h"
#include <math.h>

/* Hidden symbol for internal storage: {keystr: {k:origKey, v:value}, ...}
   Duktape objects preserve string property insertion order, so no
   separate order array is needed. */
#define MAP_STORE  DUK_HIDDEN_SYMBOL("map_store")

/* ============================================================
   Key formatting: produce a unique string for any JS value
   ============================================================ */

static void map_push_key_string(duk_context *ctx, duk_idx_t idx)
{
    idx = duk_normalize_index(ctx, idx);

    switch (duk_get_type(ctx, idx)) {
    case DUK_TYPE_UNDEFINED:
        duk_push_string(ctx, "z:undef");
        return;
    case DUK_TYPE_NULL:
        duk_push_string(ctx, "z:null");
        return;
    case DUK_TYPE_BOOLEAN:
        duk_push_sprintf(ctx, "b:%d", duk_get_boolean(ctx, idx));
        return;
    case DUK_TYPE_NUMBER: {
        double n = duk_get_number(ctx, idx);
        if (isnan(n))
            duk_push_string(ctx, "n:NaN");
        else if (isinf(n))
            duk_push_string(ctx, n > 0 ? "n:Inf" : "n:-Inf");
        else if (n == 0.0)
            duk_push_string(ctx, "n:0");  /* normalize -0 to 0 */
        else
            duk_push_sprintf(ctx, "n:%.17g", n);
        return;
    }
    case DUK_TYPE_STRING:
        /* Symbols are DUK_TYPE_STRING internally but cannot be
           string-coerced.  Use heap pointer for identity. */
        if (duk_is_symbol(ctx, idx)) {
            void *ptr = duk_get_heapptr(ctx, idx);
            duk_push_sprintf(ctx, "y:%p", ptr);
            return;
        }
        duk_push_string(ctx, "s:");
        duk_dup(ctx, idx);
        duk_concat(ctx, 2);
        return;
    default: {
#if defined(DUK_RP_USE_BIGINT)
        /* BigInts: per spec Map/Set use SameValueZero, so two
         * distinct BigInt objects with the same numeric value must
         * key identically.  Stringify the value (decimal form). */
        if (duk_rp_tval_is_bigint(duk_get_tval(ctx, idx))) {
            duk_push_string(ctx, "B:");
            duk_dup(ctx, idx);
            (void) duk_safe_to_string(ctx, -1);
            duk_concat(ctx, 2);
            return;
        }
#endif
        void *ptr = duk_get_heapptr(ctx, idx);
        duk_push_sprintf(ctx, "p:%p", ptr);
        return;
    }
    }
}

/* ============================================================
   Map methods
   ============================================================ */

/* Helper: push the store object from 'this' */
static duk_idx_t map_push_store(duk_context *ctx)
{
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, MAP_STORE);
    duk_remove(ctx, -2);
    return duk_normalize_index(ctx, -1);
}

static duk_ret_t map_set(duk_context *ctx)
{
    /* args: 0=key, 1=value */
    duk_idx_t store = map_push_store(ctx);

    map_push_key_string(ctx, 0);
    const char *ks = duk_get_string(ctx, -1);

    /* If key already exists, delete first so re-insert goes to end
       only if we want "update moves to end" behavior.
       Spec says: update does NOT change order.  So just overwrite. */

    /* Create entry {k: key, v: value} */
    duk_push_object(ctx);
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, -2, "k");
    duk_dup(ctx, 1);
    duk_put_prop_string(ctx, -2, "v");
    duk_put_prop_string(ctx, store, ks);

    duk_pop_2(ctx); /* keystr, store */

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t map_get(duk_context *ctx)
{
    duk_idx_t store = map_push_store(ctx);

    map_push_key_string(ctx, 0);
    if (duk_get_prop(ctx, store)) {
        duk_get_prop_string(ctx, -1, "v");
        return 1;
    }
    duk_push_undefined(ctx);
    return 1;
}

static duk_ret_t map_has(duk_context *ctx)
{
    duk_idx_t store = map_push_store(ctx);
    map_push_key_string(ctx, 0);
    duk_push_boolean(ctx, duk_has_prop(ctx, store));
    return 1;
}

static duk_ret_t map_delete(duk_context *ctx)
{
    duk_idx_t store = map_push_store(ctx);
    map_push_key_string(ctx, 0);
    const char *ks = duk_get_string(ctx, -1);

    if (!duk_has_prop_string(ctx, store, ks)) {
        duk_push_false(ctx);
        return 1;
    }
    duk_del_prop_string(ctx, store, ks);
    duk_push_true(ctx);
    return 1;
}

static duk_ret_t map_clear(duk_context *ctx)
{
    duk_push_this(ctx);
    duk_push_object(ctx);
    duk_put_prop_string(ctx, -2, MAP_STORE);
    duk_pop(ctx);
    return 0;
}

static duk_ret_t map_size_getter(duk_context *ctx)
{
    duk_idx_t store = map_push_store(ctx);
    int count = 0;
    duk_enum(ctx, store, DUK_ENUM_OWN_PROPERTIES_ONLY);
    while (duk_next(ctx, -1, 0)) { count++; duk_pop(ctx); }
    duk_push_int(ctx, count);
    return 1;
}

static duk_ret_t map_forEach(duk_context *ctx)
{
    /* args: 0=callback, 1=thisArg (optional) */
    REQUIRE_FUNCTION(ctx, 0, "Map.forEach: callback must be a function");
    duk_idx_t store = map_push_store(ctx);

    duk_enum(ctx, store, DUK_ENUM_OWN_PROPERTIES_ONLY);
    while (duk_next(ctx, -1, 1)) {
        /* stack: [store, enum, keystr, entry] */
        duk_get_prop_string(ctx, -1, "v");
        duk_get_prop_string(ctx, -2, "k");
        duk_remove(ctx, -3); /* remove entry */
        duk_remove(ctx, -3); /* remove keystr */
        /* stack: [store, enum, value, key] */

        /* build: callback.call(thisArg, value, key, map) */
        duk_dup(ctx, 0); /* callback */
        if (duk_is_undefined(ctx, 1))
            duk_push_undefined(ctx);
        else
            duk_dup(ctx, 1);
        duk_dup(ctx, -4); /* value */
        duk_dup(ctx, -4); /* key */
        duk_push_this(ctx);
        duk_call_method(ctx, 3);
        duk_pop(ctx); /* result */
        duk_pop_2(ctx); /* value, key */
    }
    duk_pop(ctx); /* enum */
    return 0;
}

/* ============================================================
   Iterator
   ============================================================ */

#define ITER_KEYS  DUK_HIDDEN_SYMBOL("iter_keys")
#define ITER_IDX   DUK_HIDDEN_SYMBOL("iter_idx")
#define ITER_KIND  DUK_HIDDEN_SYMBOL("iter_kind")

/* Helper: return this (for Symbol.iterator on iterators) */
static duk_ret_t map_return_this(duk_context *ctx)
{
    duk_push_this(ctx);
    return 1;
}

static duk_ret_t map_iter_next(duk_context *ctx)
{
    duk_push_this(ctx);

    duk_get_prop_string(ctx, -1, ITER_IDX);
    duk_uarridx_t idx = (duk_uarridx_t)duk_get_uint(ctx, -1);
    duk_pop(ctx);

    duk_get_prop_string(ctx, -1, ITER_KIND);
    int kind = duk_get_int(ctx, -1);
    duk_pop(ctx);

    duk_get_prop_string(ctx, -1, ITER_KEYS);
    duk_size_t len = duk_get_length(ctx, -1);
    duk_idx_t keys_idx = duk_normalize_index(ctx, -1);

    duk_push_object(ctx); /* result */

    if (idx >= (duk_uarridx_t)len) {
        duk_push_true(ctx);
        duk_put_prop_string(ctx, -2, "done");
        duk_push_undefined(ctx);
        duk_put_prop_string(ctx, -2, "value");
    } else {
        duk_push_false(ctx);
        duk_put_prop_string(ctx, -2, "done");

        /* Get the keystr, then look up in store */
        duk_get_prop_index(ctx, keys_idx, idx);
        const char *ks = duk_get_string(ctx, -1);
        duk_pop(ctx); /* keystr */

        duk_push_this(ctx);
        duk_get_prop_string(ctx, -1, MAP_STORE);
        duk_get_prop_string(ctx, -1, ks);
        /* stack: [..., result, this, store, entry] */

        if (kind == 0) {
            /* Map entries: [key, value] */
            duk_push_array(ctx);
            duk_get_prop_string(ctx, -2, "k");
            duk_put_prop_index(ctx, -2, 0);
            duk_get_prop_string(ctx, -2, "v");
            duk_put_prop_index(ctx, -2, 1);
        } else if (kind == 1) {
            /* keys */
            duk_get_prop_string(ctx, -1, "k");
        } else if (kind == 3) {
            /* Set entries: [value, value] (Set stores value in .k) */
            duk_push_array(ctx);
            duk_get_prop_string(ctx, -2, "k");
            duk_put_prop_index(ctx, -2, 0);
            duk_get_prop_string(ctx, -2, "k");
            duk_put_prop_index(ctx, -2, 1);
        } else {
            /* values */
            duk_get_prop_string(ctx, -1, "v");
        }
        /* stack: [..., result, this, store, entry, val_or_pair] */
        duk_put_prop_string(ctx, -5, "value"); /* result.value = ... */
        duk_pop_3(ctx); /* entry, store, this */

        /* Advance index */
        duk_push_this(ctx);
        duk_push_uint(ctx, idx + 1);
        duk_put_prop_string(ctx, -2, ITER_IDX);
        duk_pop(ctx);
    }

    duk_remove(ctx, -2); /* remove keys array */
    duk_remove(ctx, -2); /* remove this (from initial push) */
    return 1;
}

static void map_push_iterator(duk_context *ctx, int kind)
{
    duk_push_object(ctx);

    /* Copy store reference */
    duk_push_this(ctx);
    duk_get_prop_string(ctx, -1, MAP_STORE);
    duk_put_prop_string(ctx, -3, MAP_STORE);

    /* Snapshot the key strings into an array (so iteration is
       stable even if the map is mutated during iteration) */
    duk_get_prop_string(ctx, -1, MAP_STORE);
    duk_push_array(ctx);
    duk_uarridx_t i = 0;
    duk_enum(ctx, -2, DUK_ENUM_OWN_PROPERTIES_ONLY);
    while (duk_next(ctx, -1, 0)) {
        duk_put_prop_index(ctx, -3, i++);
    }
    duk_pop(ctx); /* enum */
    duk_remove(ctx, -2); /* remove store dup */
    duk_put_prop_string(ctx, -3, ITER_KEYS);
    duk_pop(ctx); /* this */

    /* Position and kind */
    duk_push_uint(ctx, 0);
    duk_put_prop_string(ctx, -2, ITER_IDX);
    duk_push_int(ctx, kind);
    duk_put_prop_string(ctx, -2, ITER_KIND);

    /* next() method */
    duk_push_c_function(ctx, map_iter_next, 0);
    duk_put_prop_string(ctx, -2, "next");

    /* Symbol.iterator returns self */
    duk_get_global_string(ctx, "Symbol");
    if (!duk_is_undefined(ctx, -1)) {
        duk_get_prop_string(ctx, -1, "iterator");
        if (!duk_is_undefined(ctx, -1)) {
            duk_push_c_function(ctx, map_return_this, 0);
            duk_put_prop(ctx, -4);
        } else {
            duk_pop(ctx);
        }
    }
    duk_pop(ctx); /* Symbol */
}

static duk_ret_t map_entries(duk_context *ctx)
{
    map_push_iterator(ctx, 0);
    return 1;
}

static duk_ret_t map_keys(duk_context *ctx)
{
    map_push_iterator(ctx, 1);
    return 1;
}

static duk_ret_t map_values(duk_context *ctx)
{
    map_push_iterator(ctx, 2);
    return 1;
}

static duk_ret_t set_entries(duk_context *ctx)
{
    map_push_iterator(ctx, 3);
    return 1;
}

/* ============================================================
   Map constructor
   ============================================================ */

/* Push arg[Symbol.iterator]() on the stack.  Caller is responsible
 * for popping.  Throws TypeError if the arg has no Symbol.iterator. */
static void iterable_push_iter(duk_context *ctx, duk_idx_t arg_idx)
{
    arg_idx = duk_normalize_index(ctx, arg_idx);

    /* Fetch the Symbol.iterator well-known symbol. */
    duk_get_global_string(ctx, "Symbol");
    duk_get_prop_string(ctx, -1, "iterator");
    duk_remove(ctx, -2);
    /* stack top: Symbol.iterator (the key to look up on arg) */

    /* duk_get_prop pops the key and uses obj at obj_idx.  Need stack
     * shape [..., arg, key] before the call. */
    duk_dup(ctx, arg_idx);
    duk_swap_top(ctx, -2);
    /* stack: [..., arg, Symbol.iterator] */
    duk_get_prop(ctx, -2);
    /* stack: [..., arg, arg[Symbol.iterator]] */

    if (!duk_is_callable(ctx, -1)) {
        RP_TYPE_THROW(ctx, "Map/Set: argument is not iterable");
    }

    /* duk_call_method wants [..., func, this, args...].  We have
     * [..., arg, func], so swap to put func under arg. */
    duk_swap_top(ctx, -2);
    /* stack: [..., func, arg] */
    duk_call_method(ctx, 0);
    /* stack: [..., iter] */
}

/* Iterate `iter` (already on top of stack); for each value, call
 * `cb(ctx, this_idx, value_idx)` where value_idx is the absolute
 * stack index of the iterator's current `.value`.  The callback
 * should not modify the stack beyond pushing/popping its own work;
 * the value slot will be popped after the callback returns. */
typedef void (*map_iter_cb)(duk_context *ctx, duk_idx_t this_idx, duk_idx_t value_idx);

static void map_iterate(duk_context *ctx, duk_idx_t this_idx, duk_idx_t iter_idx, map_iter_cb cb)
{
    iter_idx = duk_normalize_index(ctx, iter_idx);
    this_idx = duk_normalize_index(ctx, this_idx);
    for (;;) {
        duk_get_prop_string(ctx, iter_idx, "next");
        duk_dup(ctx, iter_idx);
        duk_call_method(ctx, 0);
        /* stack top: result */
        duk_get_prop_string(ctx, -1, "done");
        if (duk_to_boolean(ctx, -1)) {
            duk_pop_2(ctx);    /* done, result */
            break;
        }
        duk_pop(ctx);          /* done */
        duk_get_prop_string(ctx, -1, "value");
        /* stack: ..., result, value */
        cb(ctx, this_idx, duk_get_top_index(ctx));
        duk_pop_2(ctx);        /* value, result */
    }
}

static void map_ctor_cb(duk_context *ctx, duk_idx_t this_idx, duk_idx_t value_idx)
{
    /* value must be a 2-element [key, val] array (or array-like). */
    if (!duk_is_object(ctx, value_idx)) {
        RP_TYPE_THROW(ctx, "Map: iterator value must be an entry object");
    }
    duk_get_prop_index(ctx, value_idx, 0);   /* key */
    duk_get_prop_index(ctx, value_idx, 1);   /* val */
    /* call this.set(key, val) */
    duk_get_prop_string(ctx, this_idx, "set");
    duk_dup(ctx, this_idx);
    duk_dup(ctx, -4);   /* key */
    duk_dup(ctx, -4);   /* val */
    duk_call_method(ctx, 2);
    duk_pop_n(ctx, 3);  /* set retval, val, key */
}

static duk_ret_t map_constructor(duk_context *ctx)
{
    duk_idx_t this_idx;

    if (!duk_is_constructor_call(ctx))
        RP_THROW(ctx, "Map must be called with 'new'");

    duk_push_this(ctx);
    duk_push_object(ctx);
    duk_put_prop_string(ctx, -2, MAP_STORE);
    this_idx = duk_get_top_index(ctx);
    /* stack: this  (at this_idx) */

    /* Spec: no argument or null/undefined argument => empty Map. */
    if (duk_get_top(ctx) < 1 || duk_is_null_or_undefined(ctx, 0)) {
        duk_pop(ctx);
        return 0;
    }

    /* Symbol.iterator path -- handles arrays, Maps, Sets, generators,
     * and any user iterable.  Per spec ARC the spec algorithm uses
     * the iterator protocol uniformly even for arrays. */
    iterable_push_iter(ctx, 0);
    /* stack: this, iter */
    map_iterate(ctx, this_idx, duk_get_top_index(ctx), map_ctor_cb);
    duk_pop(ctx);  /* iter */
    duk_pop(ctx);  /* this */
    return 0;
}

/* ============================================================
   Set methods
   ============================================================ */

static duk_ret_t set_add(duk_context *ctx)
{
    duk_idx_t store = map_push_store(ctx);

    map_push_key_string(ctx, 0);
    const char *ks = duk_get_string(ctx, -1);

    duk_push_object(ctx);
    duk_dup(ctx, 0);
    duk_put_prop_string(ctx, -2, "k");
    duk_push_true(ctx);
    duk_put_prop_string(ctx, -2, "v");
    duk_put_prop_string(ctx, store, ks);

    duk_pop_2(ctx);

    duk_push_this(ctx);
    return 1;
}

static duk_ret_t set_forEach(duk_context *ctx)
{
    REQUIRE_FUNCTION(ctx, 0, "Set.forEach: callback must be a function");
    duk_idx_t store = map_push_store(ctx);

    duk_enum(ctx, store, DUK_ENUM_OWN_PROPERTIES_ONLY);
    while (duk_next(ctx, -1, 1)) {
        /* stack: [..., store, enum, keystr, entry] */
        duk_get_prop_string(ctx, -1, "k");
        duk_remove(ctx, -2); /* remove entry */
        duk_remove(ctx, -2); /* remove keystr */
        /* stack: [..., store, enum, value] */

        /* callback.call(thisArg, value, value, set) */
        duk_dup(ctx, 0); /* callback */
        if (duk_is_undefined(ctx, 1))
            duk_push_undefined(ctx);
        else
            duk_dup(ctx, 1);
        duk_dup(ctx, -3); /* value */
        duk_dup(ctx, -4); /* value again */
        duk_push_this(ctx);
        duk_call_method(ctx, 3);
        duk_pop(ctx); /* result */
        duk_pop(ctx); /* value */
    }
    duk_pop(ctx); /* enum */
    return 0;
}

static void set_ctor_cb(duk_context *ctx, duk_idx_t this_idx, duk_idx_t value_idx)
{
    /* call this.add(value) */
    duk_get_prop_string(ctx, this_idx, "add");
    duk_dup(ctx, this_idx);
    duk_dup(ctx, value_idx);
    duk_call_method(ctx, 1);
    duk_pop(ctx);  /* add retval */
}

static duk_ret_t set_constructor(duk_context *ctx)
{
    duk_idx_t this_idx;

    if (!duk_is_constructor_call(ctx))
        RP_THROW(ctx, "Set must be called with 'new'");

    duk_push_this(ctx);
    duk_push_object(ctx);
    duk_put_prop_string(ctx, -2, MAP_STORE);
    this_idx = duk_get_top_index(ctx);

    if (duk_get_top(ctx) < 1 || duk_is_null_or_undefined(ctx, 0)) {
        duk_pop(ctx);
        return 0;
    }

    iterable_push_iter(ctx, 0);
    map_iterate(ctx, this_idx, duk_get_top_index(ctx), set_ctor_cb);
    duk_pop(ctx);  /* iter */
    duk_pop(ctx);  /* this */
    return 0;
}

/* ============================================================
   Registration
   ============================================================ */

DUK_INTERNAL void duk_rp_install_map_set(duk_context *ctx)
{
    /* ---- Map ---- */
    duk_push_c_function(ctx, map_constructor, 1);
    duk_push_object(ctx); /* prototype */

    duk_push_c_function(ctx, map_set, 2);
    duk_put_prop_string(ctx, -2, "set");
    duk_push_c_function(ctx, map_get, 1);
    duk_put_prop_string(ctx, -2, "get");
    duk_push_c_function(ctx, map_has, 1);
    duk_put_prop_string(ctx, -2, "has");
    duk_push_c_function(ctx, map_delete, 1);
    duk_put_prop_string(ctx, -2, "delete");
    duk_push_c_function(ctx, map_clear, 0);
    duk_put_prop_string(ctx, -2, "clear");
    duk_push_c_function(ctx, map_forEach, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "forEach");
    duk_push_c_function(ctx, map_entries, 0);
    duk_put_prop_string(ctx, -2, "entries");
    duk_push_c_function(ctx, map_keys, 0);
    duk_put_prop_string(ctx, -2, "keys");
    duk_push_c_function(ctx, map_values, 0);
    duk_put_prop_string(ctx, -2, "values");

    /* size getter */
    duk_push_string(ctx, "size");
    duk_push_c_function(ctx, map_size_getter, 0);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER |
                           DUK_DEFPROP_SET_ENUMERABLE |
                           DUK_DEFPROP_SET_CONFIGURABLE);

    /* Symbol.iterator = entries */
    duk_get_global_string(ctx, "Symbol");
    if (!duk_is_undefined(ctx, -1)) {
        duk_get_prop_string(ctx, -1, "iterator");
        if (!duk_is_undefined(ctx, -1)) {
            duk_push_c_function(ctx, map_entries, 0);
            duk_put_prop(ctx, -4);
        } else {
            duk_pop(ctx);
        }
    }
    duk_pop(ctx);

    duk_put_prop_string(ctx, -2, "prototype");
    duk_put_global_string(ctx, "Map");

    /* ---- Set ---- */
    duk_push_c_function(ctx, set_constructor, 1);
    duk_push_object(ctx); /* prototype */

    duk_push_c_function(ctx, set_add, 1);
    duk_put_prop_string(ctx, -2, "add");
    duk_push_c_function(ctx, map_has, 1);
    duk_put_prop_string(ctx, -2, "has");
    duk_push_c_function(ctx, map_delete, 1);
    duk_put_prop_string(ctx, -2, "delete");
    duk_push_c_function(ctx, map_clear, 0);
    duk_put_prop_string(ctx, -2, "clear");
    duk_push_c_function(ctx, set_forEach, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "forEach");
    /* Set stores values in .k field, so use map_keys (kind=1) for values */
    duk_push_c_function(ctx, map_keys, 0);
    duk_put_prop_string(ctx, -2, "values");
    duk_push_c_function(ctx, map_keys, 0);
    duk_put_prop_string(ctx, -2, "keys");
    duk_push_c_function(ctx, set_entries, 0);
    duk_put_prop_string(ctx, -2, "entries");

    /* size getter */
    duk_push_string(ctx, "size");
    duk_push_c_function(ctx, map_size_getter, 0);
    duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_GETTER |
                           DUK_DEFPROP_SET_ENUMERABLE |
                           DUK_DEFPROP_SET_CONFIGURABLE);

    /* Symbol.iterator = values for Set */
    duk_get_global_string(ctx, "Symbol");
    if (!duk_is_undefined(ctx, -1)) {
        duk_get_prop_string(ctx, -1, "iterator");
        if (!duk_is_undefined(ctx, -1)) {
            duk_push_c_function(ctx, map_keys, 0);
            duk_put_prop(ctx, -4);
        } else {
            duk_pop(ctx);
        }
    }
    duk_pop(ctx);

    duk_put_prop_string(ctx, -2, "prototype");
    duk_put_global_string(ctx, "Set");
}

#endif  /* DUK_RP_USE_MAP_SET */
