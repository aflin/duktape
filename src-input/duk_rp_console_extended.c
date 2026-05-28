/*
 *  duk_rp_console_extended.c
 *
 *  Extended `console` binding for duktape (post duktape 2.7.0).
 *  Consolidates two pieces of work:
 *
 *    1. The minimal duk_console.c console-object installer (originally
 *       in upstream duktape's extras/console/, adapted by rampart for
 *       FLUSH / STDOUT_ONLY / STDERR_ONLY / PROXY_WRAPPER flag support).
 *
 *    2. The node-style methods that rampart layers on top:
 *       console.time/timeEnd/timeLog, console.table, console.group/
 *       groupEnd/groupCollapsed, console.count/countReset, console.clear.
 *       Implemented in JS, installed via a single duk_peval_string.
 *
 *  When DUK_RP_USE_CONSOLE_EXTENDED is defined, the heap-create
 *  auto-install pipeline calls duk_rp_install_console_extended(ctx),
 *  which performs both pieces in one go.
 *
 *  Upstream duktape's extras/console/duk_console.c is left untouched
 *  so embedders that want only the minimal binding (without rampart's
 *  additions) can still use it directly.
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_CONSOLE_EXTENDED)

#include "duk_rp_internal.h"
#include <stdio.h>
#include <stdarg.h>

/* Flags accepted by duk_rp_install_console_extended.  Matches the
 * upstream duk_console.h interface so existing rampart callers that
 * pass DUK_CONSOLE_FLUSH etc. don't need to change. */
#define DUK_CONSOLE_PROXY_WRAPPER  (1U << 0)
#define DUK_CONSOLE_FLUSH          (1U << 1)
#define DUK_CONSOLE_STDOUT_ONLY    (1U << 2)
#define DUK_CONSOLE_STDERR_ONLY    (1U << 3)

/* ============================================================
 * (1) Minimal console-object installer.
 *
 * Adapted from extras/console/duk_console.c.  Stays close to the
 * original so future upstream patches forward-merge easily.  Rampart
 * additions over upstream:
 *   - `format` hook field on the console object (user-replaceable
 *     formatter; falls back to JSON-style when absent).
 *   - FLUSH / STDOUT_ONLY / STDERR_ONLY / PROXY_WRAPPER flag set.
 *   - `exception` alias for console.error (node compat).
 * ============================================================ */

static duk_ret_t duk__console_log_helper(duk_context *ctx, const char *error_name) {
	duk_uint_t flags = (duk_uint_t) duk_get_current_magic(ctx);
	FILE *output = (flags & DUK_CONSOLE_STDOUT_ONLY) ? stdout : stderr;
	const char *out;
	duk_idx_t n = duk_get_top(ctx);
	duk_idx_t i;

	duk_get_global_string(ctx, "console");
	duk_get_prop_string(ctx, -1, "format");

	for (i = 0; i < n; i++) {
		if (duk_check_type_mask(ctx, i, DUK_TYPE_MASK_OBJECT)) {
			/* Slow path formatting. */
			duk_dup(ctx, -1);  /* console.format */
			duk_dup(ctx, i);
			duk_call(ctx, 1);
			duk_replace(ctx, i);  /* arg[i] = console.format(arg[i]); */
		}
	}

	duk_pop_2(ctx);

	duk_push_string(ctx, " ");
	duk_insert(ctx, 0);
	duk_join(ctx, n);

	if (error_name) {
		duk_push_error_object(ctx, DUK_ERR_ERROR, "%s", duk_require_string(ctx, -1));
		duk_push_string(ctx, "name");
		duk_push_string(ctx, error_name);
		duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_FORCE);  /* to get e.g. 'Trace: 1 2 3' */
		duk_get_prop_string(ctx, -1, "stack");
	}

	out = duk_to_string(ctx, -1);
	fprintf(output, "%s\n", out);
	if (flags & DUK_CONSOLE_FLUSH) {
		fflush(output);
	}
	return 0;
}

static duk_ret_t duk__console_assert(duk_context *ctx) {
	if (duk_to_boolean(ctx, 0)) {
		return 0;
	}
	duk_remove(ctx, 0);

	return duk__console_log_helper(ctx, "AssertionError");
}

static duk_ret_t duk__console_log(duk_context *ctx) {
	return duk__console_log_helper(ctx, NULL);
}

static duk_ret_t duk__console_trace(duk_context *ctx) {
	return duk__console_log_helper(ctx, "Trace");
}

static duk_ret_t duk__console_info(duk_context *ctx) {
	return duk__console_log_helper(ctx, NULL);
}

static duk_ret_t duk__console_error(duk_context *ctx) {
	return duk__console_log_helper(ctx, "Error");
}

static duk_ret_t duk__console_warn(duk_context *ctx) {
	return duk__console_log_helper(ctx, "Warning");
}

static duk_ret_t duk__console_dir(duk_context *ctx) {
	/* For now, just share the formatting of .log() */
	return duk__console_log_helper(ctx, 0);
}

static void duk__console_reg_vararg_func(duk_context *ctx, duk_c_function func, const char *name, duk_uint_t flags) {
	duk_push_c_function(ctx, func, DUK_VARARGS);
	duk_push_string(ctx, "name");
	duk_push_string(ctx, name);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_FORCE);  /* Improve stacktraces by displaying function name */
	duk_set_magic(ctx, -1, (duk_int_t) flags);
	duk_put_prop_string(ctx, -2, name);
}

static void duk__console_install_minimal(duk_context *ctx, duk_uint_t flags) {
	duk_uint_t flags_orig;

	/* If both DUK_CONSOLE_STDOUT_ONLY and DUK_CONSOLE_STDERR_ONLY where specified,
	 * just turn off DUK_CONSOLE_STDOUT_ONLY and keep DUK_CONSOLE_STDERR_ONLY.
	 */
	if ((flags & DUK_CONSOLE_STDOUT_ONLY) && (flags & DUK_CONSOLE_STDERR_ONLY)) {
		flags &= ~DUK_CONSOLE_STDOUT_ONLY;
	}
	/* Remember the original flags for future use. */
	flags_orig = flags;

	duk_push_global_object(ctx);
	duk_push_object(ctx);

	/* Custom function to format objects.  Calling code can replace
	 * this with a different formatter.
	 */
	duk_eval_string(ctx,
		"(function (E) {"
			"return function format(v){"
				"try{"
					"return E('jx',v);"
				"}catch(e){"
					"return ''+v;"
				"}"
			"};"
		"})(Duktape.enc)");
	duk_put_prop_string(ctx, -2, "format");

	flags = flags_orig;
	if (!(flags & DUK_CONSOLE_STDOUT_ONLY) && !(flags & DUK_CONSOLE_STDERR_ONLY)) {
		/* No output indicator flags were specified; these functions go to stdout. */
		flags |= DUK_CONSOLE_STDOUT_ONLY;
	}
	duk__console_reg_vararg_func(ctx, duk__console_assert, "assert", flags);
	duk__console_reg_vararg_func(ctx, duk__console_log, "log", flags);
	duk__console_reg_vararg_func(ctx, duk__console_log, "debug", flags);  /* alias to console.log */
	duk__console_reg_vararg_func(ctx, duk__console_trace, "trace", flags);
	duk__console_reg_vararg_func(ctx, duk__console_info, "info", flags);

	flags = flags_orig;
	if (!(flags & DUK_CONSOLE_STDOUT_ONLY) && !(flags & DUK_CONSOLE_STDERR_ONLY)) {
		/* No output indicator flags were specified; these functions go to stderr. */
		flags |= DUK_CONSOLE_STDERR_ONLY;
	}
	duk__console_reg_vararg_func(ctx, duk__console_warn, "warn", flags);
	duk__console_reg_vararg_func(ctx, duk__console_error, "error", flags);
	duk__console_reg_vararg_func(ctx, duk__console_error, "exception", flags);  /* alias to console.error */
	duk__console_reg_vararg_func(ctx, duk__console_dir, "dir", flags);

	duk_put_prop_string(ctx, -2, "console");

	/* Proxy wrapping: ensures any undefined console method calls are
	 * no-ops and won't cause errors.  Disabled by default because it
	 * causes confusion with examples that don't expect it.
	 */
	if (flags & DUK_CONSOLE_PROXY_WRAPPER) {
		/* Tolerate failure to initialize Proxy wrapper in case
		 * Proxy support is disabled.
		 */
		(void) duk_peval_string_noresult(ctx,
			"(function(){"
				"var D=function(){};"
				"var W={toJSON:true};"  /* whitelisted */
				"console=new Proxy(console,{"
					"get:function(t,k){"
						"var v=t[k];"
						"return typeof v==='function'||W[k]?v:D;"
					"}"
				"});"
			"})();"
		);
	}

	duk_pop(ctx);  /* pop global */
}

/* ============================================================
 * (2) Node-style extras: time / group / count / table / clear.
 *
 * Implementation lives in this JS source string.  Installs methods
 * on the existing global `console` object created by step (1).
 * ============================================================ */

static const char *duk__rp_console_extras_js =
"(function() {\n"
"  'use strict';\n"
"  var con = (typeof globalThis !== 'undefined' ? globalThis : this).console;\n"
"  if (!con) return;\n"
"\n"
"  var _timers   = {};\n"
"  var _counters = {};\n"
"  var _indent   = '';\n"
"\n"
"  function _now() {\n"
"    if (typeof performance !== 'undefined' && typeof performance.now === 'function')\n"
"      return performance.now();\n"
"    return Date.now();\n"
"  }\n"
"\n"
"  function _emit(method, args) {\n"
"    var prefix = _indent;\n"
"    if (prefix && args.length) {\n"
"      args = [prefix + (typeof args[0] === 'string' ? args[0] : String(args[0]))].concat(\n"
"        Array.prototype.slice.call(args, 1));\n"
"    }\n"
"    (con[method] || con.log).apply(con, args);\n"
"  }\n"
"\n"
"  function time(label) {\n"
"    label = (label === undefined) ? 'default' : String(label);\n"
"    if (_timers.hasOwnProperty(label)) {\n"
"      con.warn('Warning: Label \\'' + label + '\\' already exists for console.time()');\n"
"      return;\n"
"    }\n"
"    _timers[label] = _now();\n"
"  }\n"
"  function timeEnd(label) {\n"
"    label = (label === undefined) ? 'default' : String(label);\n"
"    if (!_timers.hasOwnProperty(label)) {\n"
"      con.warn('Warning: No such label \\'' + label + '\\' for console.timeEnd()');\n"
"      return;\n"
"    }\n"
"    var elapsed = _now() - _timers[label];\n"
"    delete _timers[label];\n"
"    _emit('log', [label + ': ' + elapsed.toFixed(3) + 'ms']);\n"
"  }\n"
"  function timeLog(label) {\n"
"    label = (label === undefined) ? 'default' : String(label);\n"
"    if (!_timers.hasOwnProperty(label)) {\n"
"      con.warn('Warning: No such label \\'' + label + '\\' for console.timeLog()');\n"
"      return;\n"
"    }\n"
"    var rest = Array.prototype.slice.call(arguments, 1);\n"
"    var elapsed = _now() - _timers[label];\n"
"    _emit('log', [label + ': ' + elapsed.toFixed(3) + 'ms'].concat(rest));\n"
"  }\n"
"\n"
"  function count(label) {\n"
"    label = (label === undefined) ? 'default' : String(label);\n"
"    _counters[label] = (_counters[label] || 0) + 1;\n"
"    _emit('log', [label + ': ' + _counters[label]]);\n"
"  }\n"
"  function countReset(label) {\n"
"    label = (label === undefined) ? 'default' : String(label);\n"
"    if (!_counters.hasOwnProperty(label)) {\n"
"      con.warn('Warning: Count for \\'' + label + '\\' does not exist');\n"
"      return;\n"
"    }\n"
"    _counters[label] = 0;\n"
"  }\n"
"\n"
"  function group() {\n"
"    if (arguments.length) _emit('log', Array.prototype.slice.call(arguments));\n"
"    _indent += '  ';\n"
"  }\n"
"  function groupEnd() {\n"
"    if (_indent.length >= 2) _indent = _indent.substring(0, _indent.length - 2);\n"
"  }\n"
"\n"
"  function clear() {\n"
"    /* No spec mandate for embedders; emit ANSI clear on stdout if it's a TTY,\n"
"       otherwise emit form-feed.  Either way, behaviour is best-effort. */\n"
"    try { con.log('\\u001b[2J\\u001b[H'); } catch (_e) { con.log('\\f'); }\n"
"  }\n"
"\n"
"  function table(data, columns) {\n"
"    /* Minimal node-style table: if data is array-of-objects, print each\n"
"       object as a row; otherwise fall back to .log.  Borrowed key handling\n"
"       from node's util.inspect-table fallback. */\n"
"    if (Array.isArray(data) && data.length > 0 && typeof data[0] === 'object') {\n"
"      var keys = columns || Object.keys(data[0]);\n"
"      _emit('log', ['(index)'].concat(keys).join('\\t'));\n"
"      for (var i = 0; i < data.length; i++) {\n"
"        var row = [String(i)];\n"
"        for (var k = 0; k < keys.length; k++) {\n"
"          var v = data[i][keys[k]];\n"
"          row.push(v === undefined ? '' : (typeof v === 'object' ? JSON.stringify(v) : String(v)));\n"
"        }\n"
"        _emit('log', [row.join('\\t')]);\n"
"      }\n"
"    } else {\n"
"      con.log(data);\n"
"    }\n"
"  }\n"
"\n"
"  /* Define non-enumerable so they don't show in for-in. */\n"
"  function def(name, fn) {\n"
"    Object.defineProperty(con, name,\n"
"      {value: fn, writable: true, configurable: true, enumerable: false});\n"
"  }\n"
"  def('time', time);\n"
"  def('timeEnd', timeEnd);\n"
"  def('timeLog', timeLog);\n"
"  def('count', count);\n"
"  def('countReset', countReset);\n"
"  def('group', group);\n"
"  def('groupEnd', groupEnd);\n"
"  def('groupCollapsed', group);\n"
"  def('clear', clear);\n"
"  def('table', table);\n"
"})();\n";

/* ============================================================
 * Public stitcher entry point.
 * ============================================================ */

DUK_INTERNAL void duk_rp_install_console_extended(duk_context *ctx) {
	/* Default flag set roughly matches rampart's call site
	 * (duk_console_init(ctx, DUK_CONSOLE_FLUSH)) — log goes to
	 * stdout, error/warn to stderr, flush after every call. */
	duk__console_install_minimal(ctx, DUK_CONSOLE_FLUSH);

	if (duk_peval_string(ctx, duk__rp_console_extras_js) != 0) {
		fprintf(stderr, "duk_rp_install_console_extended: extras eval failed: %s\n",
		        duk_safe_to_string(ctx, -1));
	}
	duk_pop(ctx);
}

#endif  /* DUK_RP_USE_CONSOLE_EXTENDED */
