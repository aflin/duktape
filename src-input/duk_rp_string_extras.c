/*
 *  duk_rp_string_extras.c
 *
 *  String.prototype additions:
 *    .trimStart()    ES2019
 *    .trimEnd()      ES2019
 *    .trimLeft()     pre-spec alias of .trimStart (web compat)
 *    .trimRight()    pre-spec alias of .trimEnd   (web compat)
 *    .replaceAll(s,r) ES2021
 *
 *  Gated by DUK_RP_USE_STRING_EXTRAS.  Origin: rampart's
 *  src/duktape/register.c (trim_start, trim_end, replace_all + the
 *  add_string_funcs installer).
 *
 *  replaceAll is reimplemented to use duktape's value stack as the
 *  string builder (push chunks, duk_concat) instead of rampart's
 *  external rp_string helper — keeps the file self-contained and
 *  pulls in no rampart-specific dependencies.
 *
 *  Note: String.prototype.normalize is NOT installed here -- it
 *  requires Unicode normalization tables (ICU), which would couple
 *  duktape to a 10+ MB dependency.  Rampart installs a lazy-load
 *  stub (rampart-side) that triggers `require('rampart-intl')` on
 *  first call; the real implementation lives in rampart-intl.so.
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_STRING_EXTRAS)

#include "duk_rp_internal.h"
#include <string.h>

static int duk__rp_is_trim_ws(char c) {
	return (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
	        c == '\f' || c == '\v');
}

static duk_ret_t duk__rp_string_trim_start(duk_context *ctx) {
	duk_push_this(ctx);
	const char *s = duk_to_string(ctx, -1);
	while (duk__rp_is_trim_ws(*s)) s++;
	duk_push_string(ctx, s);
	return 1;
}

static duk_ret_t duk__rp_string_trim_end(duk_context *ctx) {
	duk_push_this(ctx);
	duk_size_t len;
	const char *s = duk_to_lstring(ctx, -1, &len);
	while (len > 0 && duk__rp_is_trim_ws(s[len - 1])) len--;
	duk_push_lstring(ctx, s, len);
	return 1;
}

/* String.prototype.replaceAll(search, replacement)
 *
 * Per ES2021: if `search` is a non-global RegExp, throw TypeError.
 * We only support plain-string `search` (matches rampart's existing
 * behavior); RegExp inputs are coerced to string per ES coercion.
 *
 * Implementation: scan the haystack, push each "between-match" chunk
 * and a copy of the replacement onto the value stack, then duk_concat
 * to produce the final result.  Zero ad-hoc allocations. */
static duk_ret_t duk__rp_string_replace_all(duk_context *ctx) {
	duk_push_this(ctx);
	duk_size_t hlen;
	const char *haystack = duk_to_lstring(ctx, -1, &hlen);
	duk_size_t nlen;
	const char *needle = duk_require_lstring(ctx, 0, &nlen);
	duk_size_t rlen;
	const char *replacement = duk_require_lstring(ctx, 1, &rlen);

	if (nlen == 0) {
		/* Empty needle: spec-compliant behaviour is to insert
		 * replacement at every code-unit boundary AND before/after
		 * the whole string.  For length-N haystack the result has
		 * N+1 replacements interleaved with the haystack chars. */
		duk_idx_t pieces = 0;
		for (duk_size_t i = 0; i < hlen; i++) {
			duk_push_lstring(ctx, replacement, rlen);
			duk_push_lstring(ctx, haystack + i, 1);
			pieces += 2;
		}
		duk_push_lstring(ctx, replacement, rlen);
		pieces += 1;
		duk_concat(ctx, pieces);
		return 1;
	}

	duk_idx_t pieces = 0;
	const char *cur = haystack;
	const char *end = haystack + hlen;
	const char *run_start = cur; /* start of the current non-matching run */
	/* Fix (#3): coalesce non-matching bytes into ONE chunk per run instead of
	 * pushing one value-stack slot per byte, which made replaceAll on a large
	 * string grow the value stack to O(haystack length) and hit the valstack
	 * limit (RangeError/OOM) on ordinary multi-MB text. */
	while (cur <= end - (ptrdiff_t) nlen) {
		if (memcmp(cur, needle, nlen) == 0) {
			if (cur > run_start) {
				duk_push_lstring(ctx, run_start, (duk_size_t)(cur - run_start));
				pieces += 1;
			}
			duk_push_lstring(ctx, replacement, rlen);
			pieces += 1;
			cur += nlen;
			run_start = cur;
		} else {
			cur += 1;
		}
	}
	/* Append the trailing run (includes the last <nlen bytes that can't match). */
	if (end > run_start) {
		duk_push_lstring(ctx, run_start, (duk_size_t)(end - run_start));
		pieces += 1;
	}
	duk_concat(ctx, pieces);
	return 1;
}

DUK_INTERNAL void duk_rp_install_string_extras(duk_context *ctx) {
	duk_get_global_string(ctx, "String");
	duk_get_prop_string(ctx, -1, "prototype");

	duk_push_c_function(ctx, duk__rp_string_trim_start, 0);
	duk_put_prop_string(ctx, -2, "trimStart");
	duk_rp_set_enum_false(ctx, -1, "trimStart");

	/* trimLeft is the pre-spec name for trimStart kept around for web
	 * compat.  Same function, different prop name.  Per the ES Annex B
	 * (Additional ECMAScript Features for Web Browsers), the two MUST
	 * be the SAME function value -- not just equivalent. */
	duk_get_prop_string(ctx, -1, "trimStart");
	duk_put_prop_string(ctx, -2, "trimLeft");
	duk_rp_set_enum_false(ctx, -1, "trimLeft");

	duk_push_c_function(ctx, duk__rp_string_trim_end, 0);
	duk_put_prop_string(ctx, -2, "trimEnd");
	duk_rp_set_enum_false(ctx, -1, "trimEnd");

	duk_get_prop_string(ctx, -1, "trimEnd");
	duk_put_prop_string(ctx, -2, "trimRight");
	duk_rp_set_enum_false(ctx, -1, "trimRight");

	duk_push_c_function(ctx, duk__rp_string_replace_all, 2);
	duk_put_prop_string(ctx, -2, "replaceAll");
	duk_rp_set_enum_false(ctx, -1, "replaceAll");

	duk_pop_2(ctx);  /* pop prototype + String */
}

#endif  /* DUK_RP_USE_STRING_EXTRAS */
