/*
 *  duk_rp_object_extras.c
 *
 *  Object static method additions:
 *    Object.hasOwn(obj, prop)        ES2022
 *    Object.fromEntries(iterable)    ES2019
 *
 *  Gated by DUK_RP_USE_OBJECT_EXTRAS.  Origin: rampart's
 *  src/duktape/register.c::duk_rp_object_has_own +
 *  duk_rp_object_from_entries + add_extra_object_funcs.
 *
 *  fromEntries is extended over the rampart original: rampart only
 *  handles array-of-arrays inputs.  This version also drains an
 *  iterable (Map, Set, custom Symbol.iterator implementations), so
 *  `Object.fromEntries(new Map([['a',1]]))` works as the spec
 *  requires.
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_OBJECT_EXTRAS)

#include "duk_rp_internal.h"

static duk_ret_t duk__rp_object_has_own(duk_context *ctx) {
	duk_require_object(ctx, 0);
	const char *prop = duk_require_string(ctx, 1);
	/* Delegate to Object.prototype.hasOwnProperty for the proper
	 * own-property check (handles inherited shadowing, symbols, etc.). */
	duk_get_global_string(ctx, "Object");
	duk_get_prop_string(ctx, -1, "prototype");
	duk_get_prop_string(ctx, -1, "hasOwnProperty");
	duk_dup(ctx, 0);
	duk_push_string(ctx, prop);
	duk_call_method(ctx, 1);
	return 1;
}

static duk_ret_t duk__rp_object_from_entries(duk_context *ctx) {
	duk_push_object(ctx);
	duk_idx_t obj_idx = duk_normalize_index(ctx, -1);

	if (duk_is_array(ctx, 0)) {
		duk_size_t len = duk_get_length(ctx, 0);
		for (duk_uarridx_t i = 0; i < (duk_uarridx_t) len; i++) {
			duk_get_prop_index(ctx, 0, i);   /* entry [key, value] */
			duk_get_prop_index(ctx, -1, 0);  /* key */
			duk_get_prop_index(ctx, -2, 1);  /* value */
			duk_put_prop(ctx, obj_idx);      /* obj[key] = value */
			duk_pop(ctx);                    /* pop entry */
		}
		return 1;
	}

	/* Non-array: try Symbol.iterator-based draining.  Required for
	 * fromEntries(new Map(...)) per spec. */
	if (duk_is_object(ctx, 0)) {
		duk_get_global_string(ctx, "Symbol");
		if (!duk_is_undefined(ctx, -1)) {
			duk_get_prop_string(ctx, -1, "iterator");
			if (!duk_is_undefined(ctx, -1)) {
				duk_get_prop(ctx, 0);  /* obj[Symbol.iterator] */
				if (duk_is_function(ctx, -1)) {
					duk_dup(ctx, 0);
					duk_call_method(ctx, 0);  /* call iterator() → iter */
					duk_remove(ctx, -2);      /* drop Symbol */
					duk_idx_t iter_idx = duk_normalize_index(ctx, -1);
					for (;;) {
						duk_get_prop_string(ctx, iter_idx, "next");
						duk_dup(ctx, iter_idx);
						duk_call_method(ctx, 0);
						duk_get_prop_string(ctx, -1, "done");
						int done = duk_to_boolean(ctx, -1);
						duk_pop(ctx);
						if (done) { duk_pop(ctx); break; }
						duk_get_prop_string(ctx, -1, "value");
						duk_remove(ctx, -2);
						/* value should be [key, val] entry */
						duk_get_prop_index(ctx, -1, 0);  /* key */
						duk_get_prop_index(ctx, -2, 1);  /* value */
						duk_put_prop(ctx, obj_idx);
						duk_pop(ctx);  /* pop entry */
					}
					duk_pop(ctx);  /* iter */
					return 1;
				}
				duk_pop(ctx);
			} else {
				duk_pop(ctx);
			}
		}
		duk_pop(ctx);  /* Symbol */
	}

	return 1;  /* empty object for non-iterable, non-array inputs */
}

DUK_INTERNAL void duk_rp_install_object_extras(duk_context *ctx) {
	duk_get_global_string(ctx, "Object");

	duk_push_c_function(ctx, duk__rp_object_has_own, 2);
	duk_put_prop_string(ctx, -2, "hasOwn");
	duk_rp_set_enum_false(ctx, -1, "hasOwn");

	duk_push_c_function(ctx, duk__rp_object_from_entries, 1);
	duk_put_prop_string(ctx, -2, "fromEntries");
	duk_rp_set_enum_false(ctx, -1, "fromEntries");

	duk_pop(ctx);
}

#endif  /* DUK_RP_USE_OBJECT_EXTRAS */
