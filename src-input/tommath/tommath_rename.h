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
#define mp_2expt                         duk_rp_mp_2expt
#define mp_abs                           duk_rp_mp_abs
#define mp_add                           duk_rp_mp_add
#define mp_add_d                         duk_rp_mp_add_d
#define mp_addmod                        duk_rp_mp_addmod
#define mp_and                           duk_rp_mp_and
#define mp_clamp                         duk_rp_mp_clamp
#define mp_clear                         duk_rp_mp_clear
#define mp_clear_multi                   duk_rp_mp_clear_multi
#define mp_cmp                           duk_rp_mp_cmp
#define mp_cmp_d                         duk_rp_mp_cmp_d
#define mp_cmp_mag                       duk_rp_mp_cmp_mag
#define mp_cnt_lsb                       duk_rp_mp_cnt_lsb
#define mp_complement                    duk_rp_mp_complement
#define mp_copy                          duk_rp_mp_copy
#define mp_count_bits                    duk_rp_mp_count_bits
#define mp_decr                          duk_rp_mp_decr
#define mp_div                           duk_rp_mp_div
#define mp_div_2                         duk_rp_mp_div_2
#define mp_div_2d                        duk_rp_mp_div_2d
#define mp_div_d                         duk_rp_mp_div_d
#define mp_dr_is_modulus                 duk_rp_mp_dr_is_modulus
#define mp_dr_reduce                     duk_rp_mp_dr_reduce
#define mp_dr_setup                      duk_rp_mp_dr_setup
#define mp_error_to_string               duk_rp_mp_error_to_string
#define mp_exch                          duk_rp_mp_exch
#define mp_exptmod                       duk_rp_mp_exptmod
#define mp_expt_n                        duk_rp_mp_expt_n
#define mp_from_sbin                     duk_rp_mp_from_sbin
#define mp_from_ubin                     duk_rp_mp_from_ubin
#define mp_get_double                    duk_rp_mp_get_double
#define mp_get_i32                       duk_rp_mp_get_i32
#define mp_get_i64                       duk_rp_mp_get_i64
#define mp_get_l                         duk_rp_mp_get_l
#define mp_get_mag_u32                   duk_rp_mp_get_mag_u32
#define mp_get_mag_u64                   duk_rp_mp_get_mag_u64
#define mp_get_mag_ul                    duk_rp_mp_get_mag_ul
#define mp_grow                          duk_rp_mp_grow
#define mp_incr                          duk_rp_mp_incr
#define mp_init                          duk_rp_mp_init
#define mp_init_copy                     duk_rp_mp_init_copy
#define mp_init_i32                      duk_rp_mp_init_i32
#define mp_init_i64                      duk_rp_mp_init_i64
#define mp_init_l                        duk_rp_mp_init_l
#define mp_init_multi                    duk_rp_mp_init_multi
#define mp_init_set                      duk_rp_mp_init_set
#define mp_init_size                     duk_rp_mp_init_size
#define mp_init_u32                      duk_rp_mp_init_u32
#define mp_init_u64                      duk_rp_mp_init_u64
#define mp_init_ul                       duk_rp_mp_init_ul
#define mp_iseven                        duk_rp_mp_iseven
#define mp_isodd                         duk_rp_mp_isodd
#define mp_is_square                     duk_rp_mp_is_square
#define mp_log_n                         duk_rp_mp_log_n
#define mp_lshd                          duk_rp_mp_lshd
#define mp_mod                           duk_rp_mp_mod
#define mp_mod_2d                        duk_rp_mp_mod_2d
#define mp_mod_d                         duk_rp_mp_mod_d
#define mp_montgomery_calc_normalization duk_rp_mp_montgomery_calc_normalization
#define mp_montgomery_reduce             duk_rp_mp_montgomery_reduce
#define mp_montgomery_setup              duk_rp_mp_montgomery_setup
#define mp_mul                           duk_rp_mp_mul
#define mp_mul_2                         duk_rp_mp_mul_2
#define mp_mul_2d                        duk_rp_mp_mul_2d
#define mp_mul_d                         duk_rp_mp_mul_d
#define mp_mulmod                        duk_rp_mp_mulmod
#define mp_neg                           duk_rp_mp_neg
#define mp_or                            duk_rp_mp_or
#define mp_pack                          duk_rp_mp_pack
#define mp_pack_count                    duk_rp_mp_pack_count
#define mp_radix_size                    duk_rp_mp_radix_size
#define mp_read_radix                    duk_rp_mp_read_radix
#define mp_reduce                        duk_rp_mp_reduce
#define mp_reduce_2k                     duk_rp_mp_reduce_2k
#define mp_reduce_2k_l                   duk_rp_mp_reduce_2k_l
#define mp_reduce_2k_setup               duk_rp_mp_reduce_2k_setup
#define mp_reduce_2k_setup_l             duk_rp_mp_reduce_2k_setup_l
#define mp_reduce_is_2k                  duk_rp_mp_reduce_is_2k
#define mp_reduce_is_2k_l                duk_rp_mp_reduce_is_2k_l
#define mp_reduce_setup                  duk_rp_mp_reduce_setup
#define mp_root_n                        duk_rp_mp_root_n
#define mp_rshd                          duk_rp_mp_rshd
#define mp_sbin_size                     duk_rp_mp_sbin_size
#define mp_set                           duk_rp_mp_set
#define mp_set_double                    duk_rp_mp_set_double
#define mp_set_i32                       duk_rp_mp_set_i32
#define mp_set_i64                       duk_rp_mp_set_i64
#define mp_set_l                         duk_rp_mp_set_l
#define mp_set_u32                       duk_rp_mp_set_u32
#define mp_set_u64                       duk_rp_mp_set_u64
#define mp_set_ul                        duk_rp_mp_set_ul
#define mp_shrink                        duk_rp_mp_shrink
#define mp_signed_rsh                    duk_rp_mp_signed_rsh
#define mp_sqr                           duk_rp_mp_sqr
#define mp_sqrmod                        duk_rp_mp_sqrmod
#define mp_sqrt                          duk_rp_mp_sqrt
#define mp_sub                           duk_rp_mp_sub
#define mp_sub_d                         duk_rp_mp_sub_d
#define mp_submod                        duk_rp_mp_submod
#define mp_to_radix                      duk_rp_mp_to_radix
#define mp_to_sbin                       duk_rp_mp_to_sbin
#define mp_to_ubin                       duk_rp_mp_to_ubin
#define mp_ubin_size                     duk_rp_mp_ubin_size
#define mp_unpack                        duk_rp_mp_unpack
#define mp_xor                           duk_rp_mp_xor
#define mp_zero                          duk_rp_mp_zero
#define s_mp_add                         duk_rp_s_mp_add
#define s_mp_balance_mul                 duk_rp_s_mp_balance_mul
#define s_mp_div_3                       duk_rp_s_mp_div_3
#define s_mp_exptmod                     duk_rp_s_mp_exptmod
#define s_mp_exptmod_fast                duk_rp_s_mp_exptmod_fast
#define s_mp_get_bit                     duk_rp_s_mp_get_bit
#define s_mp_karatsuba_mul               duk_rp_s_mp_karatsuba_mul
#define s_mp_karatsuba_sqr               duk_rp_s_mp_karatsuba_sqr
#define s_mp_log                         duk_rp_s_mp_log
#define s_mp_log_2expt                   duk_rp_s_mp_log_2expt
#define s_mp_log_d                       duk_rp_s_mp_log_d
#define s_mp_montgomery_reduce_fast      duk_rp_s_mp_montgomery_reduce_fast
#define s_mp_mul_digs                    duk_rp_s_mp_mul_digs
#define s_mp_mul_digs_fast               duk_rp_s_mp_mul_digs_fast
#define s_mp_mul_high_digs               duk_rp_s_mp_mul_high_digs
#define s_mp_mul_high_digs_fast          duk_rp_s_mp_mul_high_digs_fast
#define s_mp_reverse                     duk_rp_s_mp_reverse
#define s_mp_sqr                         duk_rp_s_mp_sqr
#define s_mp_sqr_fast                    duk_rp_s_mp_sqr_fast
#define s_mp_sub                         duk_rp_s_mp_sub
#define s_mp_toom_mul                    duk_rp_s_mp_toom_mul
#define s_mp_toom_sqr                    duk_rp_s_mp_toom_sqr

/* Tunable cutoff variables (defined in bn_cutoffs.c). */
#define KARATSUBA_MUL_CUTOFF             duk_rp_KARATSUBA_MUL_CUTOFF
#define KARATSUBA_SQR_CUTOFF             duk_rp_KARATSUBA_SQR_CUTOFF
#define TOOM_MUL_CUTOFF                  duk_rp_TOOM_MUL_CUTOFF
#define TOOM_SQR_CUTOFF                  duk_rp_TOOM_SQR_CUTOFF

/* Global radix-character tables (defined in bn_mp_radix_smap.c). */
#define mp_s_rmap                        duk_rp_mp_s_rmap
#define mp_s_rmap_reverse                duk_rp_mp_s_rmap_reverse
#define mp_s_rmap_reverse_sz             duk_rp_mp_s_rmap_reverse_sz

#endif  /* DUK_RP_TOMMATH_RENAME_H_INCLUDED */
