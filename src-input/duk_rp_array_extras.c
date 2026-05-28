/*
 *  duk_rp_array_extras.c
 *
 *  ES2015-2022 Array prototype + static method additions:
 *    Array.from(iterable, mapFn?)              ES2015
 *    Array.of(...items)                        ES2015
 *    Array.prototype.find(fn)                  ES2015 (some duktape builds miss it)
 *    Array.prototype.findIndex(fn)             ES2015 (")
 *    Array.prototype.includes(x, fromIdx?)     ES2016
 *    Array.prototype.flat(depth?)              ES2019
 *    Array.prototype.flatMap(fn)               ES2019
 *    Array.prototype.at(idx)                   ES2022
 *    Array.prototype.findLast(fn)              ES2023
 *    Array.prototype.findLastIndex(fn)         ES2023
 *
 *  Gated by DUK_RP_USE_ARRAY_EXTRAS.  Origin: rampart's
 *  src/duktape/register.c, lines ~186-466 (handlers) + 728-777
 *  (installer).
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_ARRAY_EXTRAS)

#include "duk_rp_internal.h"

/* SameValueZero-ish comparison: type-match then per-type equality.
 * Different from === for NaN (NaN equals NaN here, per Array.includes
 * spec).  Object/array compared by heap pointer identity. */
static int duk__rp_compare(duk_context *ctx, duk_idx_t idx1, duk_idx_t idx2) {
	if (duk_get_type(ctx, idx1) != duk_get_type(ctx, idx2))
		return 0;
	switch (duk_get_type(ctx, idx1)) {
	case DUK_TYPE_NUMBER:
		if (duk_get_number(ctx, idx1) == duk_get_number(ctx, idx2)) return 1;
		if (duk_is_nan(ctx, idx1) && duk_is_nan(ctx, idx2)) return 1;
		break;
	case DUK_TYPE_BOOLEAN:
		if (duk_get_boolean(ctx, idx1) == duk_get_boolean(ctx, idx2)) return 1;
		break;
	case DUK_TYPE_UNDEFINED:
	case DUK_TYPE_NULL:
		return 1;
	default:
		return duk_get_heapptr(ctx, idx1) == duk_get_heapptr(ctx, idx2);
	}
	return 0;
}

static duk_ret_t duk__rp_array_includes(duk_context *ctx) {
	int i = 0, len;
	duk_push_this(ctx);
	len = duk_get_length(ctx, -1);
	if (duk_is_number(ctx, 1)) i = duk_get_int(ctx, 1);
	if (i < 0) i = 0;
	for (; i < len; i++) {
		duk_get_prop_index(ctx, -1, (duk_uarridx_t) i);
		if (duk__rp_compare(ctx, 0, -1)) {
			duk_push_true(ctx);
			return 1;
		}
		duk_pop(ctx);
	}
	duk_push_false(ctx);
	return 1;
}

static duk_ret_t duk__rp_array_find(duk_context *ctx) {
	REQUIRE_FUNCTION(ctx, 0, "Array.find - argument must be a Function");
	duk_uarridx_t i, len;
	duk_push_this(ctx);
	len = duk_get_length(ctx, -1);
	for (i = 0; i < len; i++) {
		duk_dup(ctx, 0);
		duk_get_prop_index(ctx, 1, i);
		duk_call(ctx, 1);
		duk_bool_t res = duk_to_boolean(ctx, -1);
		duk_pop(ctx);
		if (res) {
			duk_get_prop_index(ctx, 1, i);
			return 1;
		}
	}
	return 0;
}

static duk_ret_t duk__rp_array_find_index(duk_context *ctx) {
	REQUIRE_FUNCTION(ctx, 0, "Array.findIndex - argument must be a Function");
	duk_uarridx_t i, len;
	duk_push_this(ctx);
	len = duk_get_length(ctx, -1);
	for (i = 0; i < len; i++) {
		duk_dup(ctx, 0);
		duk_get_prop_index(ctx, 1, i);
		duk_call(ctx, 1);
		duk_bool_t res = duk_to_boolean(ctx, -1);
		duk_pop(ctx);
		if (res) {
			duk_push_number(ctx, (int) i);
			return 1;
		}
	}
	duk_push_number(ctx, -1);
	return 1;
}

/* Drain an iterator (already on top of stack) into the result array at
 * arr_idx, optionally mapping each value through mapFn (at stack 1).
 * Pops the iterator on exit. */
static void duk__rp_array_from_drain_iter(duk_context *ctx, duk_idx_t arr_idx, int has_map) {
	duk_idx_t iter_idx = duk_normalize_index(ctx, -1);
	duk_uarridx_t i = 0;
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
		if (has_map) {
			duk_dup(ctx, 1);
			duk_insert(ctx, -2);
			duk_push_uint(ctx, i);
			duk_call(ctx, 2);
		}
		duk_put_prop_index(ctx, arr_idx, i);
		i++;
	}
	duk_pop(ctx);
}

static duk_ret_t duk__rp_array_from(duk_context *ctx) {
	duk_idx_t top = duk_get_top(ctx);
	int has_map = (top >= 2 && duk_is_function(ctx, 1));
	duk_uarridx_t i = 0;

	duk_push_array(ctx);
	duk_idx_t arr_idx = duk_normalize_index(ctx, -1);

	if (duk_is_string(ctx, 0)) {
		/* Drive iteration via Symbol.iterator if installed so
		 * supplementary-plane code points come out as one step. */
		int handled = 0;
		duk_get_global_string(ctx, "Symbol");
		if (!duk_is_undefined(ctx, -1)) {
			duk_get_prop_string(ctx, -1, "iterator");
			if (!duk_is_undefined(ctx, -1)) {
				duk_get_prop(ctx, 0);
				if (duk_is_function(ctx, -1)) {
					duk_dup(ctx, 0);
					duk_call_method(ctx, 0);
					duk_remove(ctx, -2);
					duk__rp_array_from_drain_iter(ctx, arr_idx, has_map);
					handled = 1;
				} else {
					duk_pop(ctx);
				}
			} else {
				duk_pop(ctx);
			}
		}
		if (!handled) {
			duk_pop(ctx);
			duk_size_t slen = duk_get_length(ctx, 0);
			for (i = 0; i < (duk_uarridx_t) slen; i++) {
				duk_get_prop_index(ctx, 0, i);
				if (has_map) {
					duk_dup(ctx, 1);
					duk_insert(ctx, -2);
					duk_push_uint(ctx, i);
					duk_call(ctx, 2);
				}
				duk_put_prop_index(ctx, arr_idx, i);
			}
		}
	} else if (duk_is_array(ctx, 0)) {
		duk_size_t len = duk_get_length(ctx, 0);
		for (i = 0; i < (duk_uarridx_t) len; i++) {
			duk_get_prop_index(ctx, 0, i);
			if (has_map) {
				duk_dup(ctx, 1);
				duk_insert(ctx, -2);
				duk_push_uint(ctx, i);
				duk_call(ctx, 2);
			}
			duk_put_prop_index(ctx, arr_idx, i);
		}
	} else if (duk_is_object(ctx, 0)) {
		int has_iter = 0;
		duk_get_global_string(ctx, "Symbol");
		if (!duk_is_undefined(ctx, -1)) {
			duk_get_prop_string(ctx, -1, "iterator");
			if (!duk_is_undefined(ctx, -1)) {
				duk_get_prop(ctx, 0);
				if (duk_is_function(ctx, -1)) {
					has_iter = 1;
					duk_dup(ctx, 0);
					duk_call_method(ctx, 0);
					duk_remove(ctx, -2);
					duk__rp_array_from_drain_iter(ctx, arr_idx, has_map);
				} else {
					duk_pop(ctx);
				}
			} else {
				duk_pop(ctx);
			}
		}
		if (!has_iter) duk_pop(ctx);
		if (!has_iter) {
			if (duk_has_prop_string(ctx, 0, "length")) {
				duk_get_prop_string(ctx, 0, "length");
				duk_size_t len = (duk_size_t) duk_to_uint(ctx, -1);
				duk_pop(ctx);
				for (i = 0; i < (duk_uarridx_t) len; i++) {
					duk_get_prop_index(ctx, 0, i);
					if (has_map) {
						duk_dup(ctx, 1);
						duk_insert(ctx, -2);
						duk_push_uint(ctx, i);
						duk_call(ctx, 2);
					}
					duk_put_prop_index(ctx, arr_idx, i);
				}
			}
		}
	}
	return 1;
}

static duk_ret_t duk__rp_array_of(duk_context *ctx) {
	duk_idx_t nargs = duk_get_top(ctx);
	duk_push_array(ctx);
	for (duk_idx_t i = 0; i < nargs; i++) {
		duk_dup(ctx, i);
		duk_put_prop_index(ctx, -2, (duk_uarridx_t) i);
	}
	return 1;
}

static void duk__rp_flat_recursive(duk_context *ctx, duk_idx_t src_idx, duk_idx_t dst_idx,
                                   duk_uarridx_t *out_i, int depth) {
	duk_size_t len = duk_get_length(ctx, src_idx);
	for (duk_uarridx_t i = 0; i < (duk_uarridx_t) len; i++) {
		duk_get_prop_index(ctx, src_idx, i);
		if (depth > 0 && duk_is_array(ctx, -1)) {
			duk_idx_t sub_idx = duk_normalize_index(ctx, -1);
			duk__rp_flat_recursive(ctx, sub_idx, dst_idx, out_i, depth - 1);
			duk_pop(ctx);
		} else {
			duk_put_prop_index(ctx, dst_idx, (*out_i)++);
		}
	}
}

static duk_ret_t duk__rp_array_flat(duk_context *ctx) {
	int depth = 1;
	if (duk_is_number(ctx, 0)) {
		double d = duk_get_number(ctx, 0);
		if (d > 1000000.0) depth = 1000000;
		else depth = (int) d;
	}
	duk_push_this(ctx);
	duk_idx_t src_idx = duk_normalize_index(ctx, -1);
	duk_push_array(ctx);
	duk_idx_t dst_idx = duk_normalize_index(ctx, -1);
	duk_uarridx_t out_i = 0;
	duk__rp_flat_recursive(ctx, src_idx, dst_idx, &out_i, depth);
	return 1;
}

static duk_ret_t duk__rp_array_flat_map(duk_context *ctx) {
	REQUIRE_FUNCTION(ctx, 0, "Array.flatMap - argument must be a Function");
	duk_push_this(ctx);
	duk_idx_t this_idx = duk_normalize_index(ctx, -1);
	duk_size_t len = duk_get_length(ctx, this_idx);
	duk_push_array(ctx);
	duk_idx_t dst_idx = duk_normalize_index(ctx, -1);
	duk_uarridx_t out_i = 0;
	for (duk_uarridx_t i = 0; i < (duk_uarridx_t) len; i++) {
		duk_dup(ctx, 0);
		duk_get_prop_index(ctx, this_idx, i);
		duk_push_uint(ctx, i);
		duk_dup(ctx, this_idx);
		duk_call(ctx, 3);
		if (duk_is_array(ctx, -1)) {
			duk_idx_t sub_idx = duk_normalize_index(ctx, -1);
			duk_size_t slen = duk_get_length(ctx, sub_idx);
			for (duk_uarridx_t j = 0; j < (duk_uarridx_t) slen; j++) {
				duk_get_prop_index(ctx, sub_idx, j);
				duk_put_prop_index(ctx, dst_idx, out_i++);
			}
			duk_pop(ctx);
		} else {
			duk_put_prop_index(ctx, dst_idx, out_i++);
		}
	}
	return 1;
}

static duk_ret_t duk__rp_array_at(duk_context *ctx) {
	int idx = duk_require_int(ctx, 0);
	duk_push_this(ctx);
	int len = (int) duk_get_length(ctx, -1);
	if (idx < 0) idx += len;
	if (idx < 0 || idx >= len) return 0;
	duk_get_prop_index(ctx, -1, (duk_uarridx_t) idx);
	return 1;
}

static duk_ret_t duk__rp_array_find_last(duk_context *ctx) {
	REQUIRE_FUNCTION(ctx, 0, "Array.findLast - argument must be a Function");
	duk_push_this(ctx);
	int len = (int) duk_get_length(ctx, -1);
	for (int i = len - 1; i >= 0; i--) {
		duk_dup(ctx, 0);
		duk_get_prop_index(ctx, 1, (duk_uarridx_t) i);
		duk_push_int(ctx, i);
		duk_dup(ctx, 1);
		duk_call(ctx, 3);
		if (duk_to_boolean(ctx, -1)) {
			duk_pop(ctx);
			duk_get_prop_index(ctx, 1, (duk_uarridx_t) i);
			return 1;
		}
		duk_pop(ctx);
	}
	return 0;
}

static duk_ret_t duk__rp_array_find_last_index(duk_context *ctx) {
	REQUIRE_FUNCTION(ctx, 0, "Array.findLastIndex - argument must be a Function");
	duk_push_this(ctx);
	int len = (int) duk_get_length(ctx, -1);
	for (int i = len - 1; i >= 0; i--) {
		duk_dup(ctx, 0);
		duk_get_prop_index(ctx, 1, (duk_uarridx_t) i);
		duk_push_int(ctx, i);
		duk_dup(ctx, 1);
		duk_call(ctx, 3);
		if (duk_to_boolean(ctx, -1)) {
			duk_pop(ctx);
			duk_push_int(ctx, i);
			return 1;
		}
		duk_pop(ctx);
	}
	duk_push_int(ctx, -1);
	return 1;
}

DUK_INTERNAL void duk_rp_install_array_extras(duk_context *ctx) {
	duk_get_global_string(ctx, "Array");
	duk_get_prop_string(ctx, -1, "prototype");

	duk_push_c_function(ctx, duk__rp_array_find, 1);
	duk_put_prop_string(ctx, -2, "find");
	duk_rp_set_enum_false(ctx, -1, "find");

	duk_push_c_function(ctx, duk__rp_array_find_index, 1);
	duk_put_prop_string(ctx, -2, "findIndex");
	duk_rp_set_enum_false(ctx, -1, "findIndex");

	duk_push_c_function(ctx, duk__rp_array_includes, 2);
	duk_put_prop_string(ctx, -2, "includes");
	duk_rp_set_enum_false(ctx, -1, "includes");

	duk_push_c_function(ctx, duk__rp_array_flat, 1);
	duk_put_prop_string(ctx, -2, "flat");
	duk_rp_set_enum_false(ctx, -1, "flat");

	duk_push_c_function(ctx, duk__rp_array_flat_map, 1);
	duk_put_prop_string(ctx, -2, "flatMap");
	duk_rp_set_enum_false(ctx, -1, "flatMap");

	duk_push_c_function(ctx, duk__rp_array_at, 1);
	duk_put_prop_string(ctx, -2, "at");
	duk_rp_set_enum_false(ctx, -1, "at");

	duk_push_c_function(ctx, duk__rp_array_find_last, 1);
	duk_put_prop_string(ctx, -2, "findLast");
	duk_rp_set_enum_false(ctx, -1, "findLast");

	duk_push_c_function(ctx, duk__rp_array_find_last_index, 1);
	duk_put_prop_string(ctx, -2, "findLastIndex");
	duk_rp_set_enum_false(ctx, -1, "findLastIndex");

	duk_pop(ctx);  /* pop prototype */

	/* Static methods on Array constructor */
	duk_push_c_function(ctx, duk__rp_array_from, 2);
	duk_put_prop_string(ctx, -2, "from");
	duk_rp_set_enum_false(ctx, -1, "from");

	duk_push_c_function(ctx, duk__rp_array_of, DUK_VARARGS);
	duk_put_prop_string(ctx, -2, "of");
	duk_rp_set_enum_false(ctx, -1, "of");

	duk_pop(ctx);  /* pop Array */
}

#endif  /* DUK_RP_USE_ARRAY_EXTRAS */
