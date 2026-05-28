/*
 *  duk_rp_promise.c
 *
 *  Promise polyfill for duktape 2.7.0 (post-upstream rampart contribution).
 *
 *  Installs a global `Promise` constructor.  Polyfill body is the Taylor
 *  Hakes promise-polyfill (MIT, ~5 KB).  Self-guarding: if a Promise
 *  constructor already exists on the global object, only the missing
 *  pieces (.finally, .allSettled, .any) are patched on.
 *
 *  Depends on `setTimeout` existing at first promise-resolution time.
 *  Vanilla duktape does NOT provide setTimeout; embedders supply it
 *  (e.g., via a libevent2/libuv loop or a simple integration in their
 *  cmdline.c).  Without setTimeout, .then() callbacks throw at runtime
 *  on first scheduling.  Promise construction and chain assembly do
 *  not require setTimeout — only the eventual callback dispatch.
 *
 *  Origin: rampart's src/duktape/globals/rampart-promise.c.  Vendored
 *  here so configure.py auto-amalgamates it when DUK_RP_USE_PROMISE is
 *  defined.  Stitched into duk_create_heap() via
 *  duk_rp_install_extensions() in duk_rp_extensions_init.c.
 */

#include "duk_internal.h"

#if defined(DUK_RP_USE_PROMISE)

#include <stdio.h>  /* fprintf, stderr — for the (unlikely) install-failure path */

static const char *duk__rp_promise_polyfill_js =
    "(function(e, t) {'object' == typeof exports && 'undefined' != typeof module ? t() :'function' == typeof define && define.amd              ? define(t) :t()})(0, function() {'use strict';function e(e) {var t = this.constructor;return this.then(function(n) {return t.resolve(e()).then(function() {return n})},function(n) {return t.resolve(e()).then(function() {return t.reject(n)})})}function t(e) {return new this(function(t, n) {function r(e, n) {if (n && ('object' == typeof n || 'function' == typeof n)) {var f = n.then;if ('function' == typeof f)return void f.call(n,function(t) {r(e, t)},function(n) {o[e] = {status: 'rejected', reason: n}, 0 == --i && t(o)})}o[e] = {status: 'fulfilled', value: n}, 0 == --i && t(o)}if (!e || 'undefined' == typeof e.length)return n(new TypeError(typeof e + ' ' + e +' is not iterable(cannot read property Symbol(Symbol.iterator))'));var o = Array.prototype.slice.call(e);if (0 === o.length) return t([]);for (var i = o.length, f = 0; o.length > f; f++) r(f, o[f])})}function n(e, t) {this.name = 'AggregateError', this.errors = e, this.message = t || ''}function r(e) {var t = this;return new t(function(r, o) {if (!e || 'undefined' == typeof e.length)return o(new TypeError('Promise.any accepts an array'));var i = Array.prototype.slice.call(e);if (0 === i.length) return o(new n([],'All promises were rejected'));for (var f = [], u = 0; i.length > u; u++) try {t.resolve(i[u]).then(r)['catch'](function(e) {f.push(e),f.length === i.length && o(new n(f, 'All promises were rejected'))})} catch (c) {o(c)}})}function o(e) {return !(!e || 'undefined' == typeof e.length)}function i() {}function f(e) {if (!(this instanceof f))throw new TypeError('Promises must be constructed via new');if ('function' != typeof e) throw new TypeError('not a function');this._state = 0, this._handled = !1, this._value = undefined,this._deferreds = [], s(e, this)}function u(e, t) {for (; 3 === e._state;) e = e._value;0 !== e._state ? (e._handled = !0, f._immediateFn(function() {var n = 1 === e._state ? t.onFulfilled : t.onRejected;if (null !== n) {var r;try {r = n(e._value)} catch (o) {return void a(t.promise, o)}c(t.promise, r)} else(1 === e._state ? c : a)(t.promise, e._value)})) :e._deferreds.push(t)}function c(e, t) {try {if (t === e)throw new TypeError('A promise cannot be resolved with itself.');if (t && ('object' == typeof t || 'function' == typeof t)) {var n = t.then;if (t instanceof f) return e._state = 3, e._value = t, void l(e);if ('function' == typeof n)return void s(function(e, t) {return function() {e.apply(t, arguments)}}(n, t), e)}e._state = 1, e._value = t, l(e)} catch (r) {a(e, r)}}function a(e, t) {e._state = 2, e._value = t, l(e)}function l(e) {2 === e._state && 0 === e._deferreds.length && f._immediateFn(function() {e._handled || f._unhandledRejectionFn(e._value)});for (var t = 0, n = e._deferreds.length; n > t; t++) u(e, e._deferreds[t]);e._deferreds = null}function s(e, t) {var n = !1;try {e(function(e) {n || (n = !0, c(t, e))},function(e) {n || (n = !0, a(t, e))})} catch (r) {if (n) return;n = !0, a(t, r)}}n.prototype = Error.prototype;f.prototype['catch'] = function(e) {return this.then(null, e)}, f.prototype.then = function(e, t) {var n = new this.constructor(i);return u(this, new function(e, t, n) {this.onFulfilled = 'function' == typeof e ? e : null,this.onRejected = 'function' == typeof t ? t : null, this.promise = n}(e, t, n)), n}, f.prototype['finally'] = e, f.all = function(e) {return new f(function(t, n) {function r(e, o) {try {if (o && ('object' == typeof o || 'function' == typeof o)) {var u = o.then;if ('function' == typeof u)return void u.call(o, function(t) {r(e, t)}, n)}i[e] = o, 0 == --f && t(i)} catch (c) {n(c)}}if (!o(e)) return n(new TypeError('Promise.all accepts an array'));var i = Array.prototype.slice.call(e);if (0 === i.length) return t([]);for (var f = i.length, u = 0; i.length > u; u++) r(u, i[u])})}, f.any = r, f.allSettled = t, f.resolve = function(e) {return e && 'object' == typeof e && e.constructor === f ? e :new f(function(t) {t(e)})}, f.reject = function(e) {return new f(function(t, n) {n(e)})}, f.race = function(e) {return new f(function(t, n) {if (!o(e)) return n(new TypeError('Promise.race accepts an array'));for (var r = 0, i = e.length; i > r; r++) f.resolve(e[r]).then(t, n)})}, f._immediateFn = (function(){var q=[],s=false;function fl(){var c=q;q=[];s=false;for(var j=0;j<c.length;j++)c[j]();}return function(e){q.push(e);if(!s){s=true;setTimeout(fl,0);}};})(), f._unhandledRejectionFn = function(e) {if (typeof console === 'undefined' || !console) return;var warn = (typeof rampart === 'undefined') ? true : (rampart.warnUnhandledPromise !== false);if (warn) console.warn('Possible Unhandled Promise Rejection:', e);};var p = function() {if ('undefined' != typeof self) return self;if ('undefined' != typeof window) return window;if ('undefined' != typeof global) return global;throw Error('unable to locate global object')}();'function' != typeof p.Promise ?p.Promise = f :(p.Promise.prototype['finally'] || (p.Promise.prototype['finally'] = e),p.Promise.allSettled || (p.Promise.allSettled = t),p.Promise.any || (p.Promise.any = r))});";

DUK_INTERNAL void duk_rp_install_promise(duk_context *ctx) {
	/* The polyfill's global-object locator probes `self`, `window`,
	 * `global` (in that order).  Vanilla duktape supplies none of
	 * these.  Bind `global` to the global object before evaluating
	 * so the install can find a home for the Promise constructor.
	 * Only set it if not already defined (rampart's duk_init_context
	 * may have done it, and we shouldn't stomp). */
	duk_push_global_object(ctx);
	duk_get_prop_string(ctx, -1, "global");
	if (duk_is_undefined(ctx, -1)) {
		duk_pop(ctx);
		duk_push_global_object(ctx);
		duk_put_prop_string(ctx, -2, "global");
	} else {
		duk_pop(ctx);
	}
	duk_pop(ctx);

	if (duk_peval_string(ctx, duk__rp_promise_polyfill_js) != 0) {
		/* Polyfill failure shouldn't happen on a healthy heap.  Warn
		 * via stderr and leave Promise undefined. */
		fprintf(stderr, "duk_rp_install_promise: polyfill eval failed: %s\n",
		        duk_safe_to_string(ctx, -1));
	}
	duk_pop(ctx);
}

#endif  /* DUK_RP_USE_PROMISE */
