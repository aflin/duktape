/*
 *  duk_rp_extensions_init.c
 *
 *  Stitcher for rampart contributions (post duktape 2.7.0).
 *
 *  When at least one DUK_RP_USE_* flag is defined (the derived
 *  DUK_RP_ANY_RUNTIME guard turns on), duk_create_heap() in
 *  duk_api_heap.c calls duk_rp_install_extensions() at the end of
 *  heap construction.  This stitcher then conditionally invokes the
 *  per-feature installer for each enabled flag.  Each feature lives
 *  in its own src-input/duk_rp_<feature>.c file.
 *
 *  Install order matters: iterators (Symbol.iterator,
 *  Symbol.asyncIterator) are installed before Promise polyfill and
 *  before any code that may walk them via for-of.  ES polyfills run
 *  after iterators.  Map/Set, Blob, Buffer extras, etc. install
 *  whenever — they don't depend on each other.
 */

#include "duk_internal.h"

#if defined(DUK_RP_ANY_RUNTIME)

#if defined(DUK_RP_USE_BLOB)
DUK_INTERNAL_DECL void duk_rp_install_blob(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_CONSOLE_EXTENDED)
DUK_INTERNAL_DECL void duk_rp_install_console_extended(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_ARRAY_ITER)
DUK_INTERNAL_DECL void duk_rp_install_array_iter(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_STRING_ITER)
DUK_INTERNAL_DECL void duk_rp_install_string_iter(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_ASYNC_ITER_SYMBOL)
DUK_INTERNAL_DECL void duk_rp_install_async_iter_symbol(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_PROXY_REVOCABLE)
DUK_INTERNAL_DECL void duk_rp_install_proxy_revocable(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_MODERN_POLYFILLS)
DUK_INTERNAL_DECL void duk_rp_install_modern_polyfills(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_OBJECT_VALUES_ENTRIES)
DUK_INTERNAL_DECL void duk_rp_install_object_values_entries(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_ARRAY_EXTRAS)
DUK_INTERNAL_DECL void duk_rp_install_array_extras(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_STRING_EXTRAS)
DUK_INTERNAL_DECL void duk_rp_install_string_extras(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_OBJECT_EXTRAS)
DUK_INTERNAL_DECL void duk_rp_install_object_extras(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_BUFFER_EXTRAS)
DUK_INTERNAL_DECL void duk_rp_install_buffer_extras(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_TEXTENCODING)
DUK_INTERNAL_DECL void duk_rp_install_textencoding(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_MAP_SET)
DUK_INTERNAL_DECL void duk_rp_install_map_set(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_PROMISE)
DUK_INTERNAL_DECL void duk_rp_install_promise(duk_context *ctx);
#endif
#if defined(DUK_RP_USE_BIGINT)
DUK_INTERNAL_DECL void duk_rp_install_bigint(duk_context *ctx);
#endif

DUK_INTERNAL void duk_rp_install_extensions(duk_context *ctx) {
	/* Order matters in a couple of places:
	 *   - Buffer extras must come BEFORE TextEncoding (the WHATWG
	 *     encoder routes through Buffer's encoders).
	 *   - Map/Set + iterators come BEFORE Promise (the polyfill may
	 *     touch for-of corners in some scheduling paths).
	 *   - Console-extended assumes Buffer + TextEncoding are present
	 *     for `console.table` formatting; install console late. */
#if defined(DUK_RP_USE_BUFFER_EXTRAS)
	duk_rp_install_buffer_extras(ctx);
#endif
#if defined(DUK_RP_USE_TEXTENCODING)
	duk_rp_install_textencoding(ctx);
#endif
#if defined(DUK_RP_USE_BLOB)
	duk_rp_install_blob(ctx);
#endif
	/* Iterator-protocol installs come BEFORE Promise: the polyfill's
	 * unhandled-rejection check walks for-of corners in some paths. */
#if defined(DUK_RP_USE_ARRAY_ITER)
	duk_rp_install_array_iter(ctx);
#endif
#if defined(DUK_RP_USE_STRING_ITER)
	duk_rp_install_string_iter(ctx);
#endif
#if defined(DUK_RP_USE_ASYNC_ITER_SYMBOL)
	duk_rp_install_async_iter_symbol(ctx);
#endif
#if defined(DUK_RP_USE_PROXY_REVOCABLE)
	duk_rp_install_proxy_revocable(ctx);
#endif
#if defined(DUK_RP_USE_MODERN_POLYFILLS)
	duk_rp_install_modern_polyfills(ctx);
#endif
#if defined(DUK_RP_USE_OBJECT_VALUES_ENTRIES)
	duk_rp_install_object_values_entries(ctx);
#endif
#if defined(DUK_RP_USE_ARRAY_EXTRAS)
	duk_rp_install_array_extras(ctx);
#endif
#if defined(DUK_RP_USE_STRING_EXTRAS)
	duk_rp_install_string_extras(ctx);
#endif
#if defined(DUK_RP_USE_OBJECT_EXTRAS)
	duk_rp_install_object_extras(ctx);
#endif
#if defined(DUK_RP_USE_CONSOLE_EXTENDED)
	duk_rp_install_console_extended(ctx);
#endif
#if defined(DUK_RP_USE_MAP_SET)
	duk_rp_install_map_set(ctx);
#endif
#if defined(DUK_RP_USE_PROMISE)
	duk_rp_install_promise(ctx);
#endif
	/* BigInt installs last so its prototype can pick up Symbol.toStringTag
	 * if the Symbol global was set up elsewhere. */
#if defined(DUK_RP_USE_BIGINT)
	duk_rp_install_bigint(ctx);
#endif
}

#endif  /* DUK_RP_ANY_RUNTIME */
