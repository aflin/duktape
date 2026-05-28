/*
 *  duk_rp_object_values_entries.c
 *
 *  Install Object.values + Object.entries (ES2017).
 *
 *  Object.values is the existing rampart polyfill (register.c::
 *  add_object_values + duk_rp_values_from_object helper).
 *
 *  Object.entries is NEW in this fork — vanilla rampart has the gap
 *  (Object.entries returns `undefined` and any user call throws).
 *  Implementation mirrors values: enumerate own properties, push each
 *  as a [key, value] pair into a fresh array.
 *
 *  Gated by DUK_RP_USE_OBJECT_VALUES_ENTRIES.
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_OBJECT_VALUES_ENTRIES)

#include "duk_rp_internal.h"

static duk_ret_t duk__rp_values_from_object(duk_context *ctx, duk_idx_t idx) {
	duk_uarridx_t i = 0;

	idx = duk_normalize_index(ctx, idx);
	if (duk_is_array(ctx, idx))
		return 1;

	duk_push_array(ctx);
	if (duk_is_string(ctx, idx)) {
		const char *s = duk_get_string(ctx, idx);
		while (*s) {
			duk_push_lstring(ctx, s, 1);
			duk_put_prop_index(ctx, -2, i);
			s++;
			i++;
		}
		return 1;
	}
	if (duk_is_object(ctx, idx)) {
		duk_enum(ctx, idx, DUK_ENUM_OWN_PROPERTIES_ONLY | DUK_ENUM_NO_PROXY_BEHAVIOR);
		while (duk_next(ctx, -1, 1)) {
			duk_put_prop_index(ctx, -4, i);
			i++;
			duk_pop(ctx);
		}
		duk_pop(ctx);
		return 1;
	}
	if (duk_is_number(ctx, idx) || duk_is_buffer_data(ctx, idx))
		return 1;
	if (duk_is_undefined(ctx, idx) || duk_is_null(ctx, idx))
		RP_THROW(ctx, "Object.values - Cannot convert undefined or null to object");
	RP_THROW(ctx, "Object.values - Cannot convert to object");
	return 0;
}

static duk_ret_t duk__rp_object_values(duk_context *ctx) {
	return duk__rp_values_from_object(ctx, 0);
}

/* Object.entries(obj) — enumerate own enumerable string-keyed
 * properties and return an array of [key, value] pairs. */
static duk_ret_t duk__rp_object_entries(duk_context *ctx) {
	duk_uarridx_t i = 0;

	if (duk_is_undefined(ctx, 0) || duk_is_null(ctx, 0))
		RP_THROW(ctx, "Object.entries - Cannot convert undefined or null to object");

	duk_push_array(ctx);
	if (duk_is_object(ctx, 0) || duk_is_string(ctx, 0)) {
		duk_enum(ctx, 0, DUK_ENUM_OWN_PROPERTIES_ONLY | DUK_ENUM_NO_PROXY_BEHAVIOR);
		while (duk_next(ctx, -1, 1)) {
			/* stack: [..., result_arr, enum, key, value] */
			duk_push_array(ctx);
			/* stack: [..., result_arr, enum, key, value, pair_arr] */
			duk_dup(ctx, -3);                       /* dup key */
			duk_put_prop_index(ctx, -2, 0);         /* pair[0] = key */
			duk_dup(ctx, -2);                       /* dup value */
			duk_put_prop_index(ctx, -2, 1);         /* pair[1] = value */
			/* stack: [..., result_arr, enum, key, value, pair_arr] */
			duk_put_prop_index(ctx, -5, i++);       /* result_arr[i] = pair */
			duk_pop_2(ctx);                         /* pop key + value */
		}
		duk_pop(ctx);                               /* pop enum */
	}
	/* For numbers / buffers / arrays the spec says: treat as object,
	 * iterate enumerable own props.  Numbers have none, so empty
	 * result.  Arrays go through the duk_is_object branch above. */
	return 1;
}

DUK_INTERNAL void duk_rp_install_object_values_entries(duk_context *ctx) {
	duk_get_global_string(ctx, "Object");

	duk_push_c_function(ctx, duk__rp_object_values, 1);
	duk_put_prop_string(ctx, -2, "values");
	duk_rp_set_enum_false(ctx, -1, "values");

	duk_push_c_function(ctx, duk__rp_object_entries, 1);
	duk_put_prop_string(ctx, -2, "entries");
	duk_rp_set_enum_false(ctx, -1, "entries");

	duk_pop(ctx);
}

#endif  /* DUK_RP_USE_OBJECT_VALUES_ENTRIES */
