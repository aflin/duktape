/*
 *  duk_rp_modern_polyfills.c
 *
 *  Pure-JS polyfills for recent (ES2020 onward) standard methods that
 *  duktape lacks:
 *    String.prototype.matchAll        (ES2020)
 *    Object.groupBy                    (ES2024)
 *    Uint8Array.from / Uint8Array.of   (and the rest of the 9 TypedArray ctors)
 *    ArrayBuffer.prototype.transfer    (ES2024, partial — see note below)
 *    Array.prototype.fill              (ES2015 — still missing from duktape)
 *
 *  Note on ArrayBuffer.prototype.transfer:  This polyfill flips a
 *  JS-level _detached flag rather than truly detaching the buffer.
 *  Tests that verify byteLength becomes 0 after transfer still fail;
 *  a real detach needs a duktape engine patch.
 *
 *  Gated by DUK_RP_USE_MODERN_POLYFILLS.  Origin: rampart's
 *  src/duktape/register.c::install_modern_polyfills().
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_MODERN_POLYFILLS)

#include "duk_rp_internal.h"
#include <stdio.h>

static const char *duk__rp_modern_polyfills_js =
    "if (typeof String.prototype.matchAll !== 'function') {"
    "  Object.defineProperty(String.prototype, 'matchAll', {"
    "    configurable: true, writable: true, enumerable: false,"
    "    value: function(regex) {"
    "      if (regex != null && regex instanceof RegExp) {"
    "        if (regex.flags.indexOf('g') === -1)"
    "          throw new TypeError('String.prototype.matchAll requires a global RegExp');"
    "      }"
    "      var re = (regex != null && regex instanceof RegExp)"
    "               ? new RegExp(regex.source, regex.flags)"
    "               : new RegExp(String(regex == null ? '' : regex), 'g');"
    "      var str = String(this);"
    "      var done = false;"
    "      var iter = {"
    "        next: function() {"
    "          if (done) return { value: undefined, done: true };"
    "          var m = re.exec(str);"
    "          if (m === null) { done = true; return { value: undefined, done: true }; }"
    "          if (m[0] === '') re.lastIndex = re.lastIndex + 1;"
    "          return { value: m, done: false };"
    "        }"
    "      };"
    "      if (typeof Symbol !== 'undefined' && Symbol.iterator)"
    "        iter[Symbol.iterator] = function() { return this; };"
    "      return iter;"
    "    }"
    "  });"
    "}"
    "if (typeof Object.groupBy !== 'function') {"
    "  Object.defineProperty(Object, 'groupBy', {"
    "    configurable: true, writable: true, enumerable: false,"
    "    value: function(items, keyFn) {"
    "      if (items == null) throw new TypeError('Object.groupBy: items must be iterable');"
    "      if (typeof keyFn !== 'function') throw new TypeError('Object.groupBy: keyFn must be callable');"
    "      var result = Object.create(null);"
    "      var i = 0;"
    "      if (typeof Symbol !== 'undefined' && Symbol.iterator"
    "          && typeof items[Symbol.iterator] === 'function') {"
    "        var it = items[Symbol.iterator]();"
    "        var step;"
    "        while (!(step = it.next()).done) {"
    "          var key = keyFn(step.value, i++);"
    "          if (!result[key]) result[key] = [];"
    "          result[key].push(step.value);"
    "        }"
    "      } else {"
    "        var len = items.length >>> 0;"
    "        for (i = 0; i < len; i++) {"
    "          if (i in items) {"
    "            var k = keyFn(items[i], i);"
    "            if (!result[k]) result[k] = [];"
    "            result[k].push(items[i]);"
    "          }"
    "        }"
    "      }"
    "      return result;"
    "    }"
    "  });"
    "}"
    "['Int8Array','Uint8Array','Uint8ClampedArray','Int16Array',"
    " 'Uint16Array','Int32Array','Uint32Array','Float32Array',"
    " 'Float64Array'].forEach(function (n) {"
    "  var C = globalThis[n];"
    "  if (!C) return;"
    "  if (typeof C.from !== 'function') {"
    "    Object.defineProperty(C, 'from', {"
    "      configurable: true, writable: true,"
    "      value: function (src, mapFn, thisArg) {"
    "        var arr = Array.from(src, mapFn, thisArg);"
    "        return new this(arr);"
    "      }"
    "    });"
    "  }"
    "  if (typeof C.of !== 'function') {"
    "    Object.defineProperty(C, 'of', {"
    "      configurable: true, writable: true,"
    "      value: function () {"
    "        var a = new Array(arguments.length);"
    "        for (var i = 0; i < arguments.length; i++) a[i] = arguments[i];"
    "        return new this(a);"
    "      }"
    "    });"
    "  }"
    "});"
    "if (typeof ArrayBuffer.prototype.transfer !== 'function') {"
    "  Object.defineProperty(ArrayBuffer.prototype, 'detached', {"
    "    configurable: true,"
    "    get: function () { return this._detached === true; }"
    "  });"
    "  function __abTransfer(self, newLength) {"
    "    if (self._detached) {"
    "      var de = new TypeError('ArrayBuffer is detached');"
    "      de.name = 'TypeError'; throw de;"
    "    }"
    "    var len = (newLength === undefined) ? self.byteLength : (newLength|0);"
    "    if (len < 0) throw new RangeError('Invalid length');"
    "    var dst = new ArrayBuffer(len);"
    "    var src = new Uint8Array(self);"
    "    var view = new Uint8Array(dst);"
    "    var copy = Math.min(src.length, len);"
    "    for (var i = 0; i < copy; i++) view[i] = src[i];"
    "    Object.defineProperty(self, '_detached', "
    "      {value: true, writable: false, configurable: true, enumerable: false});"
    "    return dst;"
    "  }"
    "  Object.defineProperty(ArrayBuffer.prototype, 'transfer', {"
    "    configurable: true, writable: true,"
    "    value: function (newLength) { return __abTransfer(this, newLength); }"
    "  });"
    "  Object.defineProperty(ArrayBuffer.prototype, 'transferToFixedLength', {"
    "    configurable: true, writable: true,"
    "    value: function (newLength) { return __abTransfer(this, newLength); }"
    "  });"
    "}"
    "if (typeof Array.prototype.fill !== 'function') {"
    "  Object.defineProperty(Array.prototype, 'fill', {"
    "    configurable: true, writable: true, enumerable: false,"
    "    value: function (value, start, end) {"
    "      var len = this.length >>> 0;"
    "      var s = start === undefined ? 0 : (start | 0);"
    "      var e = end === undefined ? len : (end | 0);"
    "      if (s < 0) s = Math.max(len + s, 0); else s = Math.min(s, len);"
    "      if (e < 0) e = Math.max(len + e, 0); else e = Math.min(e, len);"
    "      for (var i = s; i < e; i++) this[i] = value;"
    "      return this;"
    "    }"
    "  });"
    "}";

DUK_INTERNAL void duk_rp_install_modern_polyfills(duk_context *ctx) {
    if (duk_peval_string(ctx, duk__rp_modern_polyfills_js) != 0) {
        fprintf(stderr, "duk_rp_install_modern_polyfills: %s\n", duk_safe_to_string(ctx, -1));
    }
    duk_pop(ctx);
}

#endif  /* DUK_RP_USE_MODERN_POLYFILLS */
