Duktape
=======

[![Build Status](https://travis-ci.org/svaarala/duktape.svg?branch=master)](https://travis-ci.org/svaarala/duktape)

Rampart Additions
-----------------

This fork of Duktape 2.7.0 is maintained for [rampart](https://rampart.dev/).
Upstream Duktape sources are unchanged; rampart features are
added as gated contributions in `src-input/duk_rp_*.c` plus a handful of
small hooks in existing `src-input/*.c` / `*.h` files.  When the
`DUK_RP_*` flags are off, the regenerated `duktape.c` is character-
identical to upstream 2.7.0.

### Building

```sh
# 1. Regenerate the amalgamation (writes to build/ by default).
cd /path/to/duktape-v270
python3 tools/configure.py

# 2. The three output files land in build/:
#      build/duktape.c        (~4.2 MB amalgamation incl. libtommath)
#      build/duktape.h        (public header)
#      build/duk_config.h     (config + rampart feature flags)
#      build/duk_source_meta.json
```

Defaults:

* `--output-directory` defaults to `build/` (gitignored).  Pass an
  explicit path to redirect elsewhere.
* `util/rp_config.h` is auto-applied as a `--fixup-file`, so the
  generated `duk_config.h` defines the `DUK_RP_USE_*` flag set under
  the `DUK_RP_ALL` umbrella.  Pass `--no-rp-config` to disable and
  produce vanilla upstream output.
* Compile the resulting `duktape.c` with `-DDUK_RP_ALL` to enable all
  rampart contributions, or define individual `DUK_RP_USE_*` flags
  for fine-grained selection.

The build requires Python 3 (the tooling was ported from upstream's
Python 2).

### Runtime additions (JS visible)

Each item is gated by a `DUK_RP_USE_*` flag in `util/rp_config.h`.

* **`DUK_RP_USE_BIGINT`** — Full ES2020 BigInt backed by a vendored
  libtommath subset in `src-input/tommath/`.  Includes `typeof === 'bigint'`,
  `123n` numeric literals, all arithmetic/bitwise/comparison/equality
  operators with spec-compliant mixed-type TypeError behavior,
  `BigInt.asIntN/asUintN/isBigInt`, `BigInt.prototype.{toString,
  toLocaleString,valueOf}`, `Object.prototype.toString.call(1n)`
  returning `[object BigInt]` via a new `DUK_HOBJECT_CLASS_BIGINT`
  slot, `JSON.stringify(BigInt)` throws TypeError, Map/Set keying
  by SameValueZero, `DataView.{get,set}BigInt64`,
  `DataView.{get,set}BigUint64`, and JS-level `BigInt64Array` /
  `BigUint64Array` typed-array classes.  Passes 119 of 141
  test262 cases against this build; the 22 known limitations are
  documented in the header comment of `src-input/duk_rp_bigint.c`.
* **`DUK_RP_USE_PROMISE`** — Promise polyfill (ES2015).
* **`DUK_RP_USE_MAP_SET`** — Map and Set (ES2015).
* **`DUK_RP_USE_TEXTENCODING`** — `TextEncoder` / `TextDecoder`
  (WHATWG).
* **`DUK_RP_USE_BLOB`** — `Blob` / `File` (W3C File API).
* **`DUK_RP_USE_BUFFER_EXTRAS`** — node-compat extras on duktape's
  Buffer.
* **`DUK_RP_USE_CONSOLE_EXTENDED`** —
  `console.{time,timeEnd,group,groupEnd,count,table,clear,...}`.
* **`DUK_RP_USE_ARRAY_ITER`** — `Array.prototype.{keys,values,entries,
  [Symbol.iterator]}`.
* **`DUK_RP_USE_STRING_ITER`** — `String.prototype[Symbol.iterator]`
  (surrogate-pair aware).
* **`DUK_RP_USE_ASYNC_ITER_SYMBOL`** — `Symbol.asyncIterator`
  well-known symbol (ES2018).
* **`DUK_RP_USE_PROXY_REVOCABLE`** — `Proxy.revocable` (ES2015).
* **`DUK_RP_USE_OBJECT_VALUES_ENTRIES`** — `Object.values`,
  `Object.entries` (ES2017).
* **`DUK_RP_USE_ARRAY_EXTRAS`** — `Array.{from,of}` and
  `Array.prototype.{includes,flat,flatMap,at,findLast,findLastIndex}`.
* **`DUK_RP_USE_STRING_EXTRAS`** — `String.prototype.{trimStart,
  trimEnd,replaceAll}`.
* **`DUK_RP_USE_OBJECT_EXTRAS`** — `Object.{hasOwn,fromEntries}`
  (ES2022).
* **`DUK_RP_USE_MODERN_POLYFILLS`** — `Object.groupBy`,
  `String.prototype.matchAll`.
* **`DUK_RP_USE_BIGINT` literal syntax** — `123n`, `0xffn`, `0o17n`,
  `0b1010n`; rejects `1.5n`, `1e10n`, legacy octal like `07n`.

### C-API additions (compile-time)

* **`DUK_RP_USE_SCOPE_VARS`** — `duk_rp_get_scope_vars()` C API for
  walking the active scope chain.
* **`DUK_RP_USE_CANCEL`** — `duk_cancel()` + cooperative bytecode
  cancellation via `LJ_TYPE_RETURN` silent unwind.  The embedder
  supplies `rp_cancel_check(udata)` / `rp_cancel_disarm()` callbacks
  that the bytecode executor polls.
* **`DUK_RP_USE_FORCE_INTERRUPT`** — `duk_force_interrupt()` C API.
* **`DUK_RP_USE_OWN_PROP_INFO`** — `duk_get_own_prop_info()` C API.

### Source layout

* `src-input/duk_rp_*.c` — rampart contributions (one file per
  feature; each starts with a header comment naming its gating flag).
* `src-input/duk_rp_internal.h` — shared helpers (`RP_THROW`,
  `REQUIRE_FUNCTION`, etc.) mirroring rampart's own conventions.
* `src-input/duk_rp_extensions_init.c` — auto-stitcher; called from
  `duk_create_heap()` to install all enabled extensions.
* `src-input/duk_rp_bigint.c` + `src-input/tommath/` — BigInt
  implementation + vendored libtommath subset (131 .c files with
  symbol-rename header so libtommath symbols don't collide with a
  second copy a downstream embedder might link).
* `util/rp_config.h` — the `DUK_RP_USE_*` flag set under the
  `DUK_RP_ALL` umbrella.  Auto-applied by `configure.py`.

### New API calls

Public C functions added by the rampart contributions.  Each is
declared in `duktape.h` only when its gating flag is on; with the
flag off the declaration disappears and the public ABI is identical
to upstream 2.7.0.

* **`duk_rp_get_scope_vars(ctx, level, type, name)`**
  (gated by `DUK_RP_USE_SCOPE_VARS`).  Walks the scope chain of the
  call-stack frame at `level` (0 = current frame, 1 = caller, ...).
  `type` selects which scope tier to enumerate, one of
  `DUK_RP_SCOPE_LOCAL`, `DUK_RP_SCOPE_CLOSURE`, `DUK_RP_SCOPE_WITH`,
  `DUK_RP_SCOPE_GLOBAL`.  If `name` is non-NULL, pushes only that
  variable's value on the stack; if NULL, pushes an object mapping
  every name visible at that tier to its current value.  Used by
  rampart's debugger / REPL integrations.

* **`duk_rp_localize(ctx, level, obj_idx, filter_arr_idx, ignore_conflicts)`**
  (gated by `DUK_RP_USE_SCOPE_VARS`).  The write-side counterpart to
  `duk_rp_get_scope_vars`.  Injects the enumerable own properties of
  the object at `obj_idx` into the declarative environment record of
  the caller at `level`, so the names become visible as if declared
  locally (any subsequent `GETVAR` resolves them after register
  lookups).  Optional Array at `filter_arr_idx` whitelists names;
  pass any non-Array index to inject all.  `ignore_conflicts`
  controls whether to silently skip names already register-bound
  (true) or throw (false).  No scope-chain mutation — reuses the
  existing env object.  Backs `rampart.localize(rampart.utils)`,
  and is a localized counterpart to `rampart.globalize()` which lifts
  `rampart.utils` into the current scope for convenience (allows for, e.g.,
  bare `printf()`, `fflush(stdout)`, etc. C-like calling syntax).

* **`duk_force_interrupt(ctx)`**
  (gated by `DUK_RP_USE_FORCE_INTERRUPT`).  Forces the bytecode
  interrupt counter to fire on the very next executed instruction
  in the target context.  Safe to call from any thread on its own
  duk_context (no shared state mutation).  Used to trip a
  cooperative break-out from inside running JS — pair with the
  `DUK_USE_EXEC_TIMEOUT_CHECK` hook to abort, longjmp, or throw.
  Requires `DUK_USE_INTERRUPT_COUNTER` (rampart's config defines it
  automatically when this flag is on).

* **`duk_get_prop_attrs(ctx, obj_idx, &out_flags)`**
  (gated by `DUK_RP_USE_OWN_PROP_INFO`).  Lightweight property
  descriptor probe.  Pops the key at the stack top and checks
  whether it is an own property of `obj_idx`.  Returns 1 on hit and
  fills `*out_flags` with a bitmask of `DUK_PROPATTR_WRITABLE`,
  `DUK_PROPATTR_ENUMERABLE`, `DUK_PROPATTR_CONFIGURABLE`,
  `DUK_PROPATTR_ACCESSOR`; returns 0 on miss.  Faster than
  `duk_get_prop_desc` (no descriptor-object allocation) — useful in
  hot paths that only need the attribute bits.

**BigInt construction / inspection** (gated by `DUK_RP_USE_BIGINT`).
A six-function surface for building and reading BigInts from C
without coupling the embedder to libtommath (no `mp_int*` in the
public signatures).

* **`duk_rp_is_bigint(ctx, idx) -> duk_bool_t`** — 1 if the value at
  `idx` is a BigInt, 0 otherwise.  Non-mutating; doesn't pop.

* **`duk_rp_push_bigint_from_i64(ctx, v)`** — push a BigInt with the
  given `int64_t` value.  Handles INT64_MIN correctly.

* **`duk_rp_push_bigint_from_double(ctx, v)`** — push a BigInt from a
  `double`.  Throws `RangeError` if the value is non-finite or
  fractional; for very large doubles only the i64-representable
  subset round-trips losslessly.

* **`duk_rp_push_bigint_from_string(ctx, s, radix)`** — parse a
  null-terminated string into a BigInt and push it.  Accepts optional
  leading sign + `0x` / `0o` / `0b` prefix (sign disallowed on
  non-decimal forms per spec).  `radix=0` auto-detects from the
  prefix; `2..36` forces.  Throws `SyntaxError` on bad input.  Empty
  or whitespace-only strings yield `0n`.

* **`duk_rp_bigint_to_double(ctx, idx) -> double`** — extract the
  BigInt at `idx` as a JavaScript `Number` via libtommath's
  `mp_get_double` (round-to-nearest).  This is the engine-internal
  conversion the `Number()` builtin uses; distinct from `ToNumber`
  which throws on BigInts.  Returns `0.0` if `idx` doesn't hold a
  BigInt.

* **`duk_rp_push_bigint_to_string(ctx, idx, radix)`** — push the
  decimal / hex / etc. string form of the BigInt at `idx`.  Bypasses
  user-overridable `BigInt.prototype.toString` and uses the
  engine-internal stringifier (`mp_to_radix`) per spec `ToString` of
  a BigInt value.  Emits lowercase digits for radix 11..36.

In addition, `DUK_RP_USE_CANCEL` activates a longjmp-based silent
unwind cooperating with the existing `DUK_USE_EXEC_TIMEOUT_CHECK`
hook.  Build with `-lpthread` when this flag is on.

This is an **embedder callback contract**, not a public C function
on the duktape side.  The embedder supplies three pieces:

1. **Polling callbacks** that the bytecode executor invokes:
   ```c
   int  rp_cancel_check(void *udata);   /* return 0 continue,
                                           1 RangeError throw,
                                           2 silent unwind */
   void rp_cancel_disarm(void);         /* clear pending cancel */
   ```
   `util/rp_config.h` wires these to `DUK_USE_EXEC_TIMEOUT_CHECK` and
   the new `DUK_USE_EXEC_TIMEOUT_DISARM` macros so the executor calls
   them automatically.

2. **An arming function** that callers use to request a cancel.
   Rampart names this `duk_cancel` for consistency with duktape's
   API style (signature below).  It's not in the duk_rp files --
   it lives in the embedder.  Pattern from rampart's `cmdline.c`:
   ```c
   /* timeout_ms = milliseconds until cancel fires; <0 disarms.
      print_cb returning 0 => silent unwind, non-zero => RangeError. */
   void duk_cancel(duk_context *ctx, int timeout_ms,
                   int (*print_cb)(void)) {
       set_deadline(timeout_ms);
       set_print_cb(print_cb);
       if (timeout_ms >= 0 && ctx)
           duk_force_interrupt(ctx);   /* duktape API, see above */
   }
   ```
   `duk_force_interrupt` (from the duktape side) ensures the
   interrupt counter fires within a few instructions rather than
   waiting for the default 256K-opcode quantum.

3. **Two supporting pieces from the duktape side** that complete
   the round-trip:

   * **`DUK_LJ_TYPE_RETURN`** — new longjmp type added to the
     executor.  When `rp_cancel_check` returns 2, the bytecode loop
     throws an internal `LJ_TYPE_RETURN` instead of an Error.  The
     longjmp handler walks any in-flight `try/finally` catchers
     (so cleanup still runs) and exits the executor normally, with
     the return value already on top of the value stack.  No
     traceback, no uncaught-error message -- the kind of clean
     exit rampart's `process.exit()` needs.

   * **`DUK_USE_EXEC_TIMEOUT_DISARM()`** — new no-arg config macro
     (see `config/config-options/DUK_USE_EXEC_TIMEOUT_DISARM.yaml`).
     The `LJ_TYPE_RETURN` handler calls this after deciding that
     an in-flight `try/finally` catcher will execute next.
     Rampart's fixup wires it to `rp_cancel_disarm()` so subsequent
     bytecode interrupts inside the finally body don't re-fire
     the cancel -- letting the finally run to completion.

Real-world example -- rampart's `process.exit()` calls
`duk_cancel(ctx, 0, rp_exit_silent_cb)` after queuing the exit
value.  Deadline 0 fires on the next instruction; the callback
returns 0 to request silent unwind; `LJ_TYPE_RETURN` unwinds with
proper try/finally semantics; the executor returns; rampart's
top-level then runs its registered exit hooks and exits the
process.

Introduction
------------

[Duktape](http://duktape.org/) is an **embeddable Javascript** engine,
with a focus on **portability** and **compact** footprint.

Duktape is easy to integrate into a C/C++ project: add `duktape.c`,
`duktape.h`, and `duk_config.h` to your build, and use the Duktape API
to call ECMAScript functions from C code and vice versa.

Main features:

* Embeddable, portable, compact
* ECMAScript E5/E5.1 compliant, with some semantics updated from ES2015+
* Partial support for ECMAScript 2015 (E6) and ECMAScript 2016 (E7),
  [Post-ES5 feature status](http://wiki.duktape.org/PostEs5Features.html),
  [kangax/compat-table](https://kangax.github.io/compat-table)
* ES2015 TypedArray and Node.js Buffer bindings
* WHATWG Encoding API living standard
* Built-in debugger
* Built-in regular expression engine
* Built-in Unicode support
* Minimal platform dependencies
* Combined reference counting and mark-and-sweep garbage collection with finalization
* Custom features like coroutines
* Property virtualization using a subset of ECMAScript ES2015 Proxy object
* Bytecode dump/load for caching compiled functions
* Distributable includes an optional logging framework, CommonJS-based module
  loading implementations, CBOR bindings, etc
* Liberal MIT license (see LICENSE.txt)

See [duktape.org](http://duktape.org/) for packaged end-user downloads
and documentation.  The end user downloads are also available from the
[duktape-releases](https://github.com/svaarala/duktape-releases) repo
as both binaries and in unpacked form as git tags.

Have fun!

Support
-------

* Duktape Wiki: [wiki.duktape.org](http://wiki.duktape.org)
* User community Q&A: Stack Overflow [duktape](http://stackoverflow.com/questions/tagged/duktape) tag
* Bugs and feature requests: [GitHub issues](https://github.com/svaarala/duktape/issues)
* General discussion: IRC `#duktape` on `chat.freenode.net` ([webchat](https://webchat.freenode.net))

About this repository
---------------------

This repository is **intended for Duktape developers only**, and contains
Duktape internals: test cases, internal documentation, sources for the
duktape.org web site, etc.

Getting started: end user
-------------------------

When embedding Duktape in your application you should use the packaged source
distributables available from [duktape.org/download.html](http://duktape.org/download.html).
See [duktape.org/guide.html#gettingstarted](http://duktape.org/guide.html#gettingstarted)
for the basics.

The distributable `src/` directory contains a `duk_config.h` configuration
header and amalgamated sources for Duktape default configuration.  If
necessary, use `python tools/configure.py` to create header and sources for
customized configuration options, see http://wiki.duktape.org/Configuring.html.
For example, to enable fastint support (example for Linux):

    $ tar xvfJ duktape-2.0.0.tar.xz
    $ cd duktape-2.0.0
    $ rm -rf src-custom
    $ python tools/configure.py \
          --source-directory src-input \
          --output-directory src-custom \
          --config-metadata config \
          -DDUK_USE_FASTINT

    # src-custom/ will now contain: duktape.c, duktape.h, duk_config.h.

You can also clone this repository, make modifications, and build a source
distributable on Linux, macOS, and Windows using `python util/dist.py`.
You'll need Python 2 and Python YAML binding.

Getting started: modifying and rebuilding the distributable
-----------------------------------------------------------

If you intend to change Duktape internals and want to rebuild the source
distributable in Linux, macOS, or Windows:

    # Linux; can often install from packages or using 'pip'
    $ sudo apt-get install python python-yaml
    $ python util/dist.py

    # macOS
    # Install Python 2.7.x
    $ pip install PyYAML
    $ python util/dist.py

    # Windows
    ; Install Python 2.7.x from python.org, and add it to PATH
    > pip install PyYAML
    > python util\dist.py

The source distributable directory will be in `dist/`.

For platform specific notes see http://wiki.duktape.org/DevelopmentSetup.html.

Getting started: other development (Linux only)
-----------------------------------------------

Other development stuff, such as building the website and running test cases,
is based on a `Makefile` **intended for Linux only**.  See detailed
instructions in http://wiki.duktape.org/DevelopmentSetup.html.

There are some Docker images which can simplify the development setup.
These are also **intended for Linux only**.  For example:

    # Build Docker images.  This takes a long time.
    $ make docker-images

    # Equivalent of 'make dist', but runs inside a container.
    $ make docker-dist-source-wd

    # Run a shell with /work/duktape containing a disposable master snapshot.
    $ make docker-shell-master

    # Run a shell with /work/duktape mounted from current directory.
    # This allows editing, building, testing, etc with an interactive
    # shell running in the container.
    $ make docker-shell-wdmount

    # For non-native images you may need:
    # https://github.com/multiarch/qemu-user-static

Branch policy
-------------

* The `master` branch is used for active development.  While pull requests
  are tested before merging, master may be broken from time to time.  When
  development on a new major release starts, master will also get API
  incompatible changes without warning.  For these reasons **you should
  generally not depend on the master branch** for building your project; use
  a release tag or a release maintenance branch instead.

* Pull requests and their related branches are frequently rebased so you
  should not fork off them.  Pull requests may be open for a while for
  testing and discussion.

* Release tags like `v1.4.1` are used for releases and match the released
  distributables.  These are stable once the release is complete.

* Maintenance branches are used for backporting fixes and features for
  maintenance releases.  Documentation changes go to master for maintenance
  releases too.  For example, `v1.5-maintenance` was created for the 1.5.0
  release and is used for 1.5.x maintenance releases.

* A maintenance branch is also created for a major release when master moves
  on to active development of the next major release.  For example,
  `v1-maintenance` was created when 1.5.0 was released (last planned 1.x
  release) and development of 2.0.0 (with API incompatible changes) started
  on master.  The 1.6.0 and 1.7.0 releases were made from `v1-maintenance`
  for example.

Versioning
----------

Duktape uses [Semantic Versioning](http://semver.org/) for official
releases.  Builds from Duktape repo are not official releases and don't
follow strict semver, mainly because `DUK_VERSION` needs to have some
compromise value that won't be strictly semver conforming.
Because Duktape tracks the latest ECMAScript specification versions,
compliance fixes are made in minor versions even when they are technically
not backwards compatible.  See
[Versioning](http://duktape.org/guide.html#versioning) for details.

Reporting bugs
--------------

See [CONTRIBUTING.md](https://github.com/svaarala/duktape/blob/master/CONTRIBUTING.md).

Security critical GitHub issues (for example anything leading to a segfault)
are tagged `security`.

Contributing
------------

See [CONTRIBUTING.md](https://github.com/svaarala/duktape/blob/master/CONTRIBUTING.md).

Copyright and license
---------------------

See [AUTHORS.rst](https://github.com/svaarala/duktape/blob/master/AUTHORS.rst)
and [LICENSE.txt](https://github.com/svaarala/duktape/blob/master/LICENSE.txt).

[Duktape Wiki](https://github.com/svaarala/duktape-wiki/) is part of Duktape
documentation and under the same copyright and license.
