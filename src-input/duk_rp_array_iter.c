/*
 *  duk_rp_array_iter.c
 *
 *  Install ES2015 Array iterator protocol:
 *    Array.prototype.keys()
 *    Array.prototype.values()
 *    Array.prototype.entries()
 *    Array.prototype[Symbol.iterator]  (= values)
 *
 *  Gated by DUK_RP_USE_ARRAY_ITER.  Origin: rampart's
 *  src/duktape/register.c::install_array_iter().
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_ARRAY_ITER)

#include "duk_rp_internal.h"
#include <stdio.h>

static const char *duk__rp_array_iter_js =
    "if (typeof Array.prototype[Symbol.iterator] !== 'function') {"
    "  var _arrIter = function(self, kind) {"
    "    var i = 0;"
    "    var iter = {"
    "      next: function() {"
    "        if (i >= self.length) return {value: undefined, done: true};"
    "        var idx = i++;"
    "        var v = (kind === 0) ? idx"
    "              : (kind === 1) ? self[idx]"
    "              :                [idx, self[idx]];"
    "        return {value: v, done: false};"
    "      }"
    "    };"
    "    iter[Symbol.iterator] = function() { return this; };"
    "    return iter;"
    "  };"
    "  var _def = function(name, fn) {"
    "    Object.defineProperty(Array.prototype, name,"
    "      {value: fn, writable: true, enumerable: false, configurable: true});"
    "  };"
    "  _def('keys',    function() { return _arrIter(this, 0); });"
    "  var _valuesFn  = function() { return _arrIter(this, 1); };"
    "  _def('values',  _valuesFn);"
    "  _def('entries', function() { return _arrIter(this, 2); });"
    "  Object.defineProperty(Array.prototype, Symbol.iterator,"
    "    {value: _valuesFn, writable: true, enumerable: false, configurable: true});"
    "}";

DUK_INTERNAL void duk_rp_install_array_iter(duk_context *ctx) {
    if (duk_peval_string(ctx, duk__rp_array_iter_js) != 0) {
        fprintf(stderr, "duk_rp_install_array_iter: %s\n", duk_safe_to_string(ctx, -1));
    }
    duk_pop(ctx);
}

#endif  /* DUK_RP_USE_ARRAY_ITER */
