/*
 *  duk_rp_async_iter_symbol.c
 *
 *  Install Symbol.asyncIterator as a well-known symbol (ES2018).
 *  Duktape ships Symbol but not this slot, so iterables that key on
 *  Symbol.asyncIterator silently set obj[undefined] without it.
 *
 *  Gated by DUK_RP_USE_ASYNC_ITER_SYMBOL.  Origin: rampart's
 *  src/duktape/register.c::install_async_iterator_symbol().
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_ASYNC_ITER_SYMBOL)

#include "duk_rp_internal.h"
#include <stdio.h>

static const char *duk__rp_async_iter_symbol_js =
    "if (typeof Symbol !== 'undefined'"
    "    && Symbol.asyncIterator === undefined) {"
    "  try { Symbol.asyncIterator = Symbol('Symbol.asyncIterator'); }"
    "  catch (_e) {}"
    "}";

DUK_INTERNAL void duk_rp_install_async_iter_symbol(duk_context *ctx) {
    if (duk_peval_string(ctx, duk__rp_async_iter_symbol_js) != 0) {
        fprintf(stderr, "duk_rp_install_async_iter_symbol: %s\n", duk_safe_to_string(ctx, -1));
    }
    duk_pop(ctx);
}

#endif  /* DUK_RP_USE_ASYNC_ITER_SYMBOL */
