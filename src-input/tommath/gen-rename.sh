#!/bin/bash
# gen-rename.sh — regenerate tommath_rename.h
#
# Run from this directory after dropping in a fresh libtommath release.
# The output prefixes every public function + global with `duk_rp_` so
# the symbols emitted into duktape's amalgamation can't collide with
# another mp_*-using library a downstream embedder might pull in.
#
# Function names are derived from the file list — each bn_*.c file in
# the keeper subset contributes exactly one symbol matching its base
# name.  Globals (cutoffs, radix map) are listed explicitly.
#
# Files in the trim list (primality, random, file I/O, GCD/LCM, modular
# inverse, deprecated compat, n-th root, ...) are not included in the
# rename header.  See tommath_superclass.h SC_JS_BIGINT block — those
# files' BN_*_C flags are undefined and their .c bodies compile to
# nothing.

set -euo pipefail
cd "$(dirname "$0")"

OUT=tommath_rename.h

TRIM_PATTERN='bn_mp_prime_|bn_s_mp_prime_|bn_prime_tab|bn_mp_rand|bn_s_mp_rand_|bn_mp_fread|bn_mp_fwrite|bn_mp_root_u32|bn_mp_invmod|bn_s_mp_invmod|bn_mp_gcd|bn_mp_lcm|bn_mp_kronecker|bn_mp_exteuclid|bn_mp_sqrtmod_prime|bn_deprecated|bn_cutoffs|bn_mp_radix_smap'

{
    cat <<'HEADER'
/*
 *  tommath_rename.h
 *
 *  Prefix every libtommath public function and global with
 *  duk_rp_ so the symbols emitted into the duktape amalgamation
 *  cannot collide with another mp_* library a downstream embedder
 *  might link (a second libtommath, libbf, mbedtls, etc.).
 *
 *  Generated mechanically by gen-rename.sh in this directory.
 *  Regenerate after upgrading libtommath.
 *
 *  Struct names (mp_int, mp_digit, etc.) and macros from tommath.h
 *  are intentionally NOT renamed — they remain visible to
 *  duk_rp_bigint.c, which is the only consumer.
 */

#if !defined(DUK_RP_TOMMATH_RENAME_H_INCLUDED)
#define DUK_RP_TOMMATH_RENAME_H_INCLUDED

/* Functions (one per bn_*.c file in the keeper set). */
HEADER

    ls bn_*.c 2>/dev/null \
      | grep -vE "$TRIM_PATTERN" \
      | sed 's/^bn_//;s/\.c$//' \
      | sort -u \
      | awk '{ printf "#define %-32s duk_rp_%s\n", $1, $1 }'

    cat <<'CUTOFFS'

/* Tunable cutoff variables (defined in bn_cutoffs.c). */
CUTOFFS

    for v in KARATSUBA_MUL_CUTOFF KARATSUBA_SQR_CUTOFF TOOM_MUL_CUTOFF TOOM_SQR_CUTOFF ; do
        printf '#define %-32s duk_rp_%s\n' "$v" "$v"
    done

    cat <<'RMAP'

/* Global radix-character tables (defined in bn_mp_radix_smap.c). */
RMAP

    for v in mp_s_rmap mp_s_rmap_reverse mp_s_rmap_reverse_sz ; do
        printf '#define %-32s duk_rp_%s\n' "$v" "$v"
    done

    echo ''
    echo '#endif  /* DUK_RP_TOMMATH_RENAME_H_INCLUDED */'
} > "$OUT"

echo "Wrote $OUT ($(wc -l < "$OUT") lines)"
