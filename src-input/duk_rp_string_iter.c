/*
 *  duk_rp_string_iter.c
 *
 *  Install ES2015 String.prototype[Symbol.iterator] on duktape, with
 *  surrogate-pair-aware iteration so supplementary-plane codepoints
 *  yield as a single step instead of two surrogate halves.
 *
 *  Gated by DUK_RP_USE_STRING_ITER.  Origin: rampart's
 *  src/duktape/register.c::install_string_iter().
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_STRING_ITER)

#include "duk_rp_internal.h"
#include <stdio.h>

static const char *duk__rp_string_iter_js =
    "if (typeof String.prototype[Symbol.iterator] !== 'function') {"
    "  Object.defineProperty(String.prototype, Symbol.iterator, {"
    "    writable: true, enumerable: false, configurable: true,"
    "    value: function() {"
    "      var s = String(this), i = 0;"
    "      var iter = {"
    "        next: function() {"
    "          if (i >= s.length) return {value: undefined, done: true};"
    "          var c = s.charCodeAt(i);"
    "          if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.length) {"
    "            var lo = s.charCodeAt(i + 1);"
    "            if (lo >= 0xDC00 && lo <= 0xDFFF) {"
    "              var v = s.substring(i, i + 2);"
    "              i += 2;"
    "              return {value: v, done: false};"
    "            }"
    "          }"
    "          return {value: s.charAt(i++), done: false};"
    "        }"
    "      };"
    "      iter[Symbol.iterator] = function() { return this; };"
    "      return iter;"
    "    }"
    "  });"
    "}";

DUK_INTERNAL void duk_rp_install_string_iter(duk_context *ctx) {
    if (duk_peval_string(ctx, duk__rp_string_iter_js) != 0) {
        fprintf(stderr, "duk_rp_install_string_iter: %s\n", duk_safe_to_string(ctx, -1));
    }
    duk_pop(ctx);
}

#endif  /* DUK_RP_USE_STRING_ITER */
