/*
 *  duk_rp_object_extras.c
 *
 *  Object static method additions:
 *    Object.hasOwn(obj, prop)              ES2022
 *    Object.fromEntries(iterable)          ES2019
 *    Object.getOwnPropertyDescriptors(obj) ES2017
 *
 *  Plus in-place spec-compliance fixes (also gated by this flag) for
 *  property-key handling in upstream duktape 2.7.0 built-ins:
 *    - Object.assign: include Symbol-keyed enumerable own props
 *      (duk_bi_object.c:69 -- adds DUK_ENUM_INCLUDE_SYMBOLS)
 *    - Reflect.{get,set,has,deleteProperty}: accept Symbol keys
 *      (duk_bi_reflect.c -- replaces duk_to_string with
 *      duk_to_property_key_hstring, per spec ToPropertyKey)
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

/* Object.getOwnPropertyDescriptors(obj) — ES2017 19.1.2.9.
 *
 * Returns an object whose own enumerable string-keyed properties
 * correspond to the descriptors of obj's own properties (including
 * Symbol-keyed and non-enumerable ones).  This is the inverse of
 * Object.defineProperties.
 *
 * Spec algorithm (per ES2022 20.1.2.10):
 *   1. obj = ToObject(O)
 *   2. ownKeys = obj.[[OwnPropertyKeys]]()
 *   3. descriptors = OrdinaryObjectCreate(%Object.prototype%)
 *   4. For each key in ownKeys:
 *        desc = obj.[[GetOwnProperty]](key)
 *        descriptor = FromPropertyDescriptor(desc)
 *        if descriptor is not undefined:
 *            CreateDataPropertyOrThrow(descriptors, key, descriptor)
 *   5. Return descriptors
 *
 * Note: ownKeys includes BOTH string and Symbol keys, AND
 * non-enumerable own properties.  The descriptor's own
 * `enumerable` field carries that information.
 */
static duk_ret_t duk__rp_object_get_own_property_descriptors(duk_context *ctx) {
	duk_require_object(ctx, 0);

	/* Result object — Object.prototype is already its prototype via
	 * the default duk_push_object. */
	duk_push_object(ctx);  /* idx 1: result */

	/* Enumerate all own keys: strings + symbols + non-enumerable. */
	duk_enum(ctx, 0, DUK_ENUM_OWN_PROPERTIES_ONLY |
	                 DUK_ENUM_INCLUDE_NONENUMERABLE |
	                 DUK_ENUM_INCLUDE_SYMBOLS);

	while (duk_next(ctx, -1, 0 /*get_value=0; just the key*/)) {
		/* Stack: [ obj result enum key ] */
		duk_dup(ctx, -1);
		/* Stack: [ obj result enum key keyCopy ] */
		duk_get_prop_desc(ctx, 0, 0 /*flags*/);
		/* Pops the keyCopy from top, pushes descriptor (or undefined). */
		/* Stack: [ obj result enum key descriptor ] */
		if (!duk_is_undefined(ctx, -1)) {
			/* result[key] = descriptor.  duk_put_prop pops both key and
			 * value, so swap them to put_prop's expected (key,value) order. */
			duk_put_prop(ctx, 1);
			/* Stack: [ obj result enum ] */
		} else {
			duk_pop_2(ctx);  /* drop undefined desc + key */
		}
	}
	duk_pop(ctx);  /* enum */
	return 1;       /* result */
}

DUK_INTERNAL void duk_rp_install_object_extras(duk_context *ctx) {
	duk_get_global_string(ctx, "Object");

	duk_push_c_function(ctx, duk__rp_object_has_own, 2);
	duk_put_prop_string(ctx, -2, "hasOwn");
	duk_rp_set_enum_false(ctx, -1, "hasOwn");

	duk_push_c_function(ctx, duk__rp_object_from_entries, 1);
	duk_put_prop_string(ctx, -2, "fromEntries");
	duk_rp_set_enum_false(ctx, -1, "fromEntries");

	duk_push_c_function(ctx, duk__rp_object_get_own_property_descriptors, 1);
	duk_put_prop_string(ctx, -2, "getOwnPropertyDescriptors");
	duk_rp_set_enum_false(ctx, -1, "getOwnPropertyDescriptors");

	duk_pop(ctx);
}

#endif  /* DUK_RP_USE_OBJECT_EXTRAS */
