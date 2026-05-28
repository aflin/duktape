/*
 *  duk_rp_internal.h
 *
 *  Shared helpers used by all rampart contribution source files
 *  (src-input/duk_rp_*.c).  These mirror the small RP_THROW / etc.
 *  macros from rampart's src/include/rampart.h so the ported files
 *  stay close to their originals and forward-merging rampart changes
 *  stays cheap.
 *
 *  This header is unconditional (no DUK_RP_USE_* guard) because it's
 *  only a set of preprocessor macros — they don't emit any code
 *  unless used.  When no rampart features are enabled, the per-file
 *  bodies that would use them are themselves #if'd out, so nothing
 *  references these macros and they evaporate.
 *
 *  Include in addition to duk_internal.h.  Do NOT include via the
 *  public duktape.h — these are amalgamation-internal macros, not
 *  part of the rampart public API.
 */

#if !defined(DUK_RP_INTERNAL_H_INCLUDED)
#define DUK_RP_INTERNAL_H_INCLUDED

#include "duk_internal.h"

#define RP_THROW(ctx,...) do {                                       \
    duk_push_error_object((ctx), DUK_ERR_ERROR, __VA_ARGS__);        \
    (void) duk_throw(ctx);                                           \
} while(0)

#define RP_SYNTAX_THROW(ctx,...) do {                                \
    duk_push_error_object((ctx), DUK_ERR_SYNTAX_ERROR, __VA_ARGS__); \
    (void) duk_throw(ctx);                                           \
} while(0)

#define RP_TYPE_THROW(ctx,...) do {                                  \
    duk_push_error_object((ctx), DUK_ERR_TYPE_ERROR, __VA_ARGS__);   \
    (void) duk_throw(ctx);                                           \
} while(0)

#define RP_RANGE_THROW(ctx,...) do {                                 \
    duk_push_error_object((ctx), DUK_ERR_RANGE_ERROR, __VA_ARGS__);  \
    (void) duk_throw(ctx);                                           \
} while(0)

/* Argument-type guards used by ported rampart sources.  The GCC
 * statement-expression form ({ ... }) matches rampart.h verbatim. */
#define REQUIRE_FUNCTION(ctx,idx,...) ({                             \
    duk_idx_t __rp_i = (idx);                                        \
    if (!duk_is_function((ctx), __rp_i)) {                           \
        RP_THROW((ctx), __VA_ARGS__);                                \
    }                                                                \
})

#define REQUIRE_STRING(ctx,idx,...) ({                               \
    duk_idx_t __rp_i = (idx);                                        \
    if (!duk_is_string((ctx), __rp_i)) {                             \
        RP_THROW((ctx), __VA_ARGS__);                                \
    }                                                                \
})

#define REQUIRE_INT(ctx,idx,...) ({                                  \
    duk_idx_t __rp_i = (idx);                                        \
    if (!duk_is_number((ctx), __rp_i)) {                             \
        RP_THROW((ctx), __VA_ARGS__);                                \
    }                                                                \
    int __rp_r = duk_get_int((ctx), __rp_i);                         \
    __rp_r;                                                          \
})

#define REQUIRE_STR_OR_BUF(ctx, idx, sz, ...) ({                     \
    const char *__rp_r = NULL;                                       \
    duk_idx_t __rp_i = (idx);                                        \
    if (duk_is_string((ctx), __rp_i))                                \
        __rp_r = duk_get_lstring((ctx), __rp_i, (sz));               \
    else if (duk_is_buffer_data((ctx), __rp_i))                      \
        __rp_r = (const char *) duk_get_buffer_data((ctx), __rp_i, (sz));\
    else                                                             \
        RP_THROW((ctx), __VA_ARGS__);                                \
    __rp_r;                                                          \
})

/* Mark `propname` on the object at `objidx` as non-enumerable.  Used
 * by buffer/blob/etc. to hide internal property names from for-in. */
static DUK_INLINE void duk_rp_set_enum_false(duk_context *ctx, duk_idx_t objidx, const char *propname) {
    objidx = duk_normalize_index(ctx, objidx);
    duk_push_string(ctx, propname);
    duk_def_prop(ctx, objidx, DUK_DEFPROP_CLEAR_ENUMERABLE);
}

#endif  /* DUK_RP_INTERNAL_H_INCLUDED */
