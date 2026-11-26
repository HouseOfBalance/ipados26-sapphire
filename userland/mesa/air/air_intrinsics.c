/*
 * Copyright © 2026 Darwin on redwood contributors
 * SPDX-License-Identifier: MIT
 */

/*
 * The air.* functions. There are 125 distinct declarations in the corpus but
 * their names are <operation>.<type suffix>, so this is a table of about 50
 * operations in four families (docs/metal.md 9.2). The suffix is not trusted
 * for anything: the types come from the call's own LLVM types, and the suffix
 * is only what picks the handler.
 *
 * What each name means is not guessed: it is read off Apple's own compiler.
 * AGXCompilerCore carries the whole vocabulary as a table of names at
 * 0x16d082..0x16d400 of the library in this device's shared cache (the list is
 * in docs/air.md section 3.2), and that table distinguishes exactly the
 * functions the Metal Shading Language Specification defines -- round from
 * rint, exp from exp2, fmod from mod, pow from powr. Two of this file's
 * handlers were guesses before that table was read; one of them was wrong.
 */

#include "air_private.h"

#include <string.h>

#include "nir_builtin_builder.h"
#include "util/u_math.h"

/* the dot-separated parts of an air.* name, after "air." */
#define AIR_MAX_PARTS 8
/* 48 and not 32: air.normalize_function_constant_predicate's first part is 37
 * characters, and at 32 split_name refused the whole name rather than the
 * operation -- "a name this cannot take apart", which reads like a malformed
 * module and was a buffer. It is the longest part in Apple's own vocabulary
 * list (section 3.2); the longest multi-part name there,
 * air.next_intersection_query.instancing.triangle_data, has no part over 14.
 */
struct air_name {
   unsigned num;
   char part[AIR_MAX_PARTS][48];
};

static bool
split_name(const char *name, struct air_name *out)
{
   out->num = 0;
   out->part[0][0] = 0;
   const char *p = name + 4; /* past "air." */
   /* "air." with nothing after it names no operation. Without this the loop
    * below never runs, split_name returns true all the same, and the
    * dispatcher compares a part[0] nothing wrote -- which gcc points out as
    * soon as the first comparison is made unconditionally.
    */
   if (!*p)
      return false;
   while (*p && out->num < AIR_MAX_PARTS) {
      const char *dot = strchr(p, '.');
      size_t len = dot ? (size_t)(dot - p) : strlen(p);
      if (len >= sizeof(out->part[0]))
         return false;
      memcpy(out->part[out->num], p, len);
      out->part[out->num][len] = 0;
      out->num++;
      if (!dot)
         return true;
      p = dot + 1;
   }
   return out->num < AIR_MAX_PARTS;
}

static bool
is_signed_part(const char *s)
{
   return !strcmp(s, "s");
}

/* Metal's fmod: the remainder of x/y, with the SIGN OF x, correctly rounded.
 *
 * nir_frem is not that function. Its constant expression is, literally,
 *
 *     src0 - src1 * truncf(src0 / src1)                (nir_opcodes.py)
 *
 * and ir3 lowers it to the same three instructions (ir3_compiler.c sets
 * .lower_fmod). Two things are wrong with the formula, and
 * tools/air/intrincheck.py counted both: of 264 cases of air.fmod.f32 it
 * reported 47 wrong, 25 of them a sign and 22 of them a number.
 *
 *   - THE SIGN OF A ZERO IS LOST. fmod(-0, 5) is -0 and fmod(-1.5, 0.5) is
 *     -0, and the subtraction gives +0 for both: -0 - (0.5 * -0) is -0 - -0,
 *     which is +0. This is the shape of the air.sign bug recorded further
 *     down this file -- a sign of zero that a formula does not carry -- and
 *     it is the half of this that is not about precision at all.
 *   - THE PRODUCT IS ROUNDED BEFORE THE SUBTRACTION. y * trunc(x/y) needs up
 *     to 48 significant bits and a float has 24, so the subtraction is exact
 *     but of the wrong product: fmod(1.5, 0x1.fffffep-2) came out 1.19e-07
 *     against 8.94e-08, one whole ulp of the dividend, and fmod(1e8, 3) came
 *     out 0 where the answer is 1. A number, not a sign.
 *
 * Whose definition is followed here, and why it is Metal's: the Metal Shading
 * Language Specification's table entry for fmod gives the same subtraction
 * formula, but it inherits OpenCL's accuracy table along with it, where fmod
 * is 0 ULP -- an exactly specified function, not a licensed approximation --
 * and OpenCL's edge cases are C99 Annex F's, which say fmod(+-0, y) is +-0.
 * Two references on this machine agree with that reading and none with the
 * formula: this machine's libm answers -0 for fmodf(-0, 0.5) and for
 * fmodf(-1.5, 0.5), and LLVM's own `frem` -- JIT-executed by lli, which is
 * the second engine and not a reading of a document -- answers -0 for both
 * and for frem(-2.5, -0.5). The corpus itself says nothing either way: not
 * one of its 128 libraries contains a single `frem` instruction, because
 * Apple's compiler always emits the call.
 *
 * So:
 *
 *   q = trunc(x / y)
 *   r = fma(-y, q, x)        one rounding, not two
 *   if (|r| >= |y|)   r -= copysign(|y|, r)      q was one too small
 *   if (sign r != sign x) r += copysign(|y|, x)  q was one too large
 *   if (r == 0)       r  = copysign(0, x)        a zero keeps x's sign
 *
 * The fma is the whole of the precision argument: when q is an exact integer
 * the true remainder x - q*y is itself exactly representable (that is why
 * IEEE 754 can require `remainder` to be exact), so ONE rounding of it is no
 * rounding at all. The two corrections are there because fl(x/y) can round
 * across an integer and move trunc by one either way, and because ir3 does
 * not divide: it lowers fdiv to fmul(a, frcp(b)) and frcp is approximate, so
 * a quotient off by one has to be survivable rather than assumed away.
 *
 * WHAT THIS STILL DOES NOT DO, measured and not guessed: q must be exactly
 * representable, which means |x/y| < 2^24 for a float and 2^11 for a half.
 * Above that no correction of a bounded size can help -- fmod(1e8, 1e-8)
 * wants a quotient of 1e16 -- and the exact answer needs an iterated
 * reduction: scale y up by 2^k so the partial quotient stays under 2^23,
 * subtract, repeat. Each pass removes only 23 - 1 bits of the exponent
 * difference, that difference reaches 253 for two normal floats, so twelve
 * passes of about ten instructions each would be the price, and its exactness
 * ON THE ADRENO could not be established from this machine at all for the
 * frcp reason above. It is not done here; section 4 of docs/air.md carries
 * the condition and the measurement.
 *
 * Over the reference pool this leaves 7 of 264 cases differing, every one of
 * them with |x/y| >= 2^24; over 195 906 random pairs of normal floats every
 * single disagreement with this machine's libm has |x/y| >= 2^24, and over
 * 376 438 random pairs whose quotient IS representable there is none.
 */
static nir_def *
air_fmod(nir_builder *b, nir_def *x, nir_def *y)
{
   uint64_t sign_bit = 1ull << (x->bit_size - 1);
   /* The sign of a float is taken and planted with integer bit operations and
    * never with a multiply by zero or a copysign built out of one: this
    * builder runs with exact off (Metal's fast library), and
    * nir_opt_algebraic's inexact rule (('~fmul', a, 0.0), 0.0) would throw
    * away exactly the bit being carried. iand and ior cannot be rewritten by
    * a float rule because they are not float operations.
    */
   nir_def *sx = nir_iand_imm(b, x, sign_bit);
   nir_def *ay = nir_fabs(b, y);
   nir_def *y_as_x = nir_ior(b, ay, sx);              /* copysign(|y|, x) */

   nir_def *q = nir_ftrunc(b, nir_fdiv(b, x, y));
   nir_def *r = nir_ffma(b, nir_fneg(b, y), q, x);

   /* |r| >= |y|: the quotient was one too small. A NaN fails this comparison
    * (an ordered fge is false for one) and fails the feq against zero at the
    * end; the middle correction may well fire for a NaN, and adding to a NaN
    * gives a NaN, so a NaN comes out of all three unchanged.
    */
   nir_def *y_as_r = nir_ior(b, ay, nir_iand_imm(b, r, sign_bit));
   r = nir_bcsel(b, nir_fge(b, nir_fabs(b, r), ay), nir_fsub(b, r, y_as_r), r);

   /* r's sign is not x's: the quotient was one too large */
   nir_def *wrong_sign = nir_ine(b, nir_iand_imm(b, r, sign_bit), sx);
   r = nir_bcsel(b, nir_iand(b, wrong_sign, nir_fneu_imm(b, r, 0.0)),
                 nir_fadd(b, r, y_as_x), r);

   /* and a zero result is the dividend's zero */
   return nir_bcsel(b, nir_feq_imm(b, r, 0.0), sx, r);
}

/* -- the families ------------------------------------------------------- */

static bool
math_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n, bool *handled)
{
   const char *op = n->part[0];
   unsigned argc = in->num_operands - 1;

   /* the families this file does not handle are named before the arguments
    * are touched, so that their error says so rather than complaining about
    * a texture handle used as a number
    */
   if (strstr(op, "texture") || strstr(op, "_depth_") || !strncmp(op, "sample", 6) ||
       !strncmp(op, "gather", 6) || !strncmp(op, "read_", 5) || !strncmp(op, "write_", 6) ||
       strstr(op, "_lod_")) {
      *handled = false;
      return true;
   }

   nir_def *a[4] = { 0 };
   for (unsigned i = 0; i < argc && i < 4; i++)
      if (!(a[i] = air_get_ssa(c, in->operands[1 + i])))
         return false;

   bool is_float = air_type_is_float(air_type_scalar(in->base.type));
   /* the sign of an integer operation is the last-but-one part: air.min.s.i32 */
   bool is_signed = n->num >= 3 ? is_signed_part(n->part[1]) : true;
   nir_def *r = NULL;
   *handled = true;

   /* fast math is what every one of these means: Metal's "fast" library */
   bool was_exact = c->b.exact;
   c->b.exact = false;

#define NEED(k) \
   do { \
      if (argc < (k)) { \
         c->b.exact = was_exact; \
         return air_error(c, "intrinsic: air.%s of %u arguments, wanting %u", op, argc, (unsigned)(k)); \
      } \
   } while (0)

   if (!strcmp(op, "fast_fabs") || !strcmp(op, "fabs")) {
      NEED(1);
      r = nir_fabs(&c->b, a[0]);
   } else if (!strcmp(op, "fast_sqrt") || !strcmp(op, "sqrt")) {
      NEED(1);
      r = nir_fsqrt(&c->b, a[0]);
   } else if (!strcmp(op, "fast_rsqrt") || !strcmp(op, "rsqrt")) {
      NEED(1);
      r = nir_frsq(&c->b, a[0]);
   } else if (!strcmp(op, "fast_floor") || !strcmp(op, "floor")) {
      NEED(1);
      r = nir_ffloor(&c->b, a[0]);
   } else if (!strcmp(op, "fast_ceil") || !strcmp(op, "ceil")) {
      NEED(1);
      r = nir_fceil(&c->b, a[0]);
   } else if (!strcmp(op, "fast_round") || !strcmp(op, "round")) {
      NEED(1);
      /* round() takes a half away from zero; rint() takes it to even, and
       * Apple's compiler has both air.fast_round and air.fast_rint, so this
       * one is not the even one. (It was nir_fround_even here until the
       * vocabulary table said otherwise.)
       *
       * Truncate, then step one away from zero if the remainder reaches a
       * half. Not trunc(x + copysign(0.5, x)): that addition rounds for an x
       * just under a half and answers 1 where the answer is 0. x - trunc(x)
       * is exact for every float, so this needs no magnitude guard, and it
       * keeps the sign of a negative zero. Checked bit-exact against C's
       * roundf over the halves, the sub-halves, both zeros and 2^24.
       */
      nir_def *t = nir_ftrunc(&c->b, a[0]);
      nir_def *d = nir_fabs(&c->b, nir_fsub(&c->b, a[0], t));
      r = nir_bcsel(&c->b, nir_fge_imm(&c->b, d, 0.5),
                    nir_fadd(&c->b, t, nir_fsign(&c->b, a[0])), t);
   } else if (!strcmp(op, "fast_rint") || !strcmp(op, "rint")) {
      NEED(1);
      r = nir_fround_even(&c->b, a[0]);
   } else if (!strcmp(op, "fast_trunc") || !strcmp(op, "trunc")) {
      NEED(1);
      r = nir_ftrunc(&c->b, a[0]);
   } else if (!strcmp(op, "fast_fract") || !strcmp(op, "fract")) {
      NEED(1);
      r = nir_ffract(&c->b, a[0]);
   } else if (!strcmp(op, "fast_sin") || !strcmp(op, "sin")) {
      NEED(1);
      r = nir_fsin(&c->b, a[0]);
   } else if (!strcmp(op, "fast_cos") || !strcmp(op, "cos")) {
      NEED(1);
      r = nir_fcos(&c->b, a[0]);
   } else if (!strcmp(op, "fast_tan") || !strcmp(op, "tan")) {
      NEED(1);
      r = nir_fdiv(&c->b, nir_fsin(&c->b, a[0]), nir_fcos(&c->b, a[0]));
   } else if (!strcmp(op, "fast_atan2") || !strcmp(op, "atan2")) {
      NEED(2);
      /* Mesa's nir_atan2 is right everywhere but the origin, where its own
       * comment says it deliberately pretends 0/0 = 1 because GLSL licenses an
       * implementation to deviate from IEEE at (0, 0). That gives
       * atan2(0, 0) = 3*pi/4. Metal's atan2 is OpenCL's, which keeps the IEEE
       * values:
       *
       *    atan2(+-0, x > 0) = +-0        atan2(+-0, +0) = +-0
       *    atan2(+-0, x < 0) = +-pi       atan2(+-0, -0) = +-pi
       *
       * so a shader taking the angle of a vector that happens to be zero gets
       * 135 degrees instead of 0. That is not a precision deviation of the
       * kind the fast_ prefix allows, so the origin is handled here and the
       * rest left to Mesa.
       *
       * The sign of a zero cannot be had from a float comparison -- -0 == 0 --
       * so the sign bit is read as an integer. A NIR value is a bit vector and
       * ilt reinterprets it; nir_atan2 avoids this only to match the GLSL
       * front-end, which is not a constraint here.
       */
      nir_def *y = a[0], *x = a[1];
      nir_def *origin = nir_iand(&c->b, nir_feq_imm(&c->b, y, 0.0),
                                 nir_feq_imm(&c->b, x, 0.0));
      nir_def *mag = nir_fmul_imm(&c->b,
                                  nir_b2fN(&c->b, nir_ilt_imm(&c->b, x, 0),
                                           y->bit_size),
                                  M_PI);
      nir_def *at_origin =
         nir_bcsel(&c->b, nir_ilt_imm(&c->b, y, 0), nir_fneg(&c->b, mag), mag);
      r = nir_bcsel(&c->b, origin, at_origin, nir_atan2(&c->b, y, x));
   } else if (!strcmp(op, "fast_asin") || !strcmp(op, "asin") ||
              !strcmp(op, "fast_acos") || !strcmp(op, "acos")) {
      NEED(1);
      nir_def *x = a[0];
      nir_def *root = nir_fsqrt(&c->b,
         nir_fmul(&c->b, nir_fsub(&c->b, nir_imm_floatN_t(&c->b, 1.0, x->bit_size), x),
                  nir_fadd_imm(&c->b, x, 1.0)));
      bool is_asin = !strcmp(op, "fast_asin") || !strcmp(op, "asin");
      r = is_asin ? nir_atan2(&c->b, x, root) : nir_atan2(&c->b, root, x);
   } else if (!strcmp(op, "fast_atan") || !strcmp(op, "atan")) {
      NEED(1);
      r = nir_atan(&c->b, a[0]);
   } else if (!strcmp(op, "fast_sinh") || !strcmp(op, "sinh")) {
      NEED(1);
      unsigned bits = a[0]->bit_size;
      nir_def *big = nir_fge_imm(&c->b, nir_fabs(&c->b, a[0]),
                                 bits > 16 ? 0x1p-12 : 0x1p-5);
      r = nir_bcsel(&c->b, big,
                    nir_fmul_imm(&c->b,
                                 nir_fsub(&c->b, nir_fexp(&c->b, a[0]),
                                          nir_fexp(&c->b, nir_fneg(&c->b, a[0]))), 0.5),
                    a[0]);
   } else if (!strcmp(op, "fast_cosh") || !strcmp(op, "cosh")) {
      NEED(1);
      r = nir_fmul_imm(&c->b, nir_fadd(&c->b, nir_fexp(&c->b, a[0]),
                                       nir_fexp(&c->b, nir_fneg(&c->b, a[0]))), 0.5);
   } else if (!strcmp(op, "fast_tanh") || !strcmp(op, "tanh")) {
      NEED(1);
      unsigned bits = a[0]->bit_size;
      nir_def *x = nir_fmin(&c->b, nir_fmax(&c->b, a[0],
                                            nir_imm_floatN_t(&c->b, bits > 16 ? -10.0 : -4.2, bits)),
                            nir_imm_floatN_t(&c->b, bits > 16 ? 10.0 : 4.2, bits));
      nir_def *ex = nir_fexp(&c->b, x), *enx = nir_fexp(&c->b, nir_fneg(&c->b, x));
      bool was = c->b.exact;
      c->b.exact = true;
      nir_def *regular = nir_fge_imm(&c->b, nir_fabs(&c->b, a[0]),
                                     bits > 16 ? 0x1p-12 : 0x1p-5);
      c->b.exact = was;
      r = nir_bcsel(&c->b, regular,
                    nir_fdiv(&c->b, nir_fsub(&c->b, ex, enx), nir_fadd(&c->b, ex, enx)),
                    a[0]);
   } else if (!strcmp(op, "fast_exp") || !strcmp(op, "exp")) {
      NEED(1);
      /* the natural one: Apple's compiler names exp and exp2 separately */
      r = nir_fexp2(&c->b, nir_fmul_imm(&c->b, a[0], M_LOG2E));
   } else if (!strcmp(op, "fast_exp2") || !strcmp(op, "exp2")) {
      NEED(1);
      r = nir_fexp2(&c->b, a[0]);
   } else if (!strcmp(op, "fast_exp10") || !strcmp(op, "exp10")) {
      NEED(1);
      r = nir_fexp2(&c->b, nir_fmul_imm(&c->b, a[0], 3.321928094887362)); /* log2(10) */
   } else if (!strcmp(op, "fast_log") || !strcmp(op, "log")) {
      NEED(1);
      r = nir_fmul_imm(&c->b, nir_flog2(&c->b, a[0]), 1.0 / M_LOG2E);
   } else if (!strcmp(op, "fast_log2") || !strcmp(op, "log2")) {
      NEED(1);
      r = nir_flog2(&c->b, a[0]);
   } else if (!strcmp(op, "fast_log10") || !strcmp(op, "log10")) {
      NEED(1);
      r = nir_fmul_imm(&c->b, nir_flog2(&c->b, a[0]), 0.30102999566398120); /* 1/log2(10) */
   } else if (!strcmp(op, "fast_pow") || !strcmp(op, "pow") ||
              !strcmp(op, "fast_powr") || !strcmp(op, "powr")) {
      NEED(2);
      /* powr is pow with x >= 0 promised, which is what nir_fpow computes */
      r = nir_fpow(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "fast_fmod") || !strcmp(op, "fmod") ||
              !strcmp(op, "fast_mod") || !strcmp(op, "mod")) {
      NEED(2);
      r = air_fmod(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "fast_divide") || !strcmp(op, "divide")) {
      NEED(2);
      r = nir_fdiv(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "saturate") || !strcmp(op, "fast_saturate")) {
      NEED(1);
      r = nir_fsat(&c->b, a[0]);
   } else if (!strcmp(op, "sign") && is_float) {
      NEED(1);
      nir_def *x = a[0];
      nir_def *pos = nir_b2fN(&c->b, nir_fgt_imm(&c->b, x, 0.0), x->bit_size);
      nir_def *neg = nir_b2fN(&c->b, nir_flt_imm(&c->b, x, 0.0), x->bit_size);
      r = nir_bcsel(&c->b, nir_feq_imm(&c->b, x, 0.0), x,
                    nir_fsub(&c->b, pos, neg));
   } else if (!strcmp(op, "sign")) {
      NEED(1);
      r = nir_isign(&c->b, a[0]);   /* the float case is above */
   } else if (!strcmp(op, "mix")) {
      NEED(3);
      /* mix(x, y, a) = x + a * (y - x), and that is what Metal defines */
      r = nir_fadd(&c->b, a[0],
                   nir_fmul(&c->b, a[2], nir_fsub(&c->b, a[1], a[0])));
   } else if (!strcmp(op, "dot")) {
      NEED(2);
      r = nir_fdot(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "fast_fmin") || !strcmp(op, "fmin") || !strcmp(op, "min")) {
      NEED(2);
      r = is_float ? nir_fmin(&c->b, a[0], a[1])
                   : (is_signed ? nir_imin(&c->b, a[0], a[1]) : nir_umin(&c->b, a[0], a[1]));
   } else if (!strcmp(op, "fast_fmax") || !strcmp(op, "fmax") || !strcmp(op, "max")) {
      NEED(2);
      r = is_float ? nir_fmax(&c->b, a[0], a[1])
                   : (is_signed ? nir_imax(&c->b, a[0], a[1]) : nir_umax(&c->b, a[0], a[1]));
   } else if (!strcmp(op, "fast_clamp") || !strcmp(op, "clamp")) {
      NEED(3);
      if (is_float)
         r = nir_fmin(&c->b, nir_fmax(&c->b, a[0], a[1]), a[2]);
      else if (is_signed)
         r = nir_imin(&c->b, nir_imax(&c->b, a[0], a[1]), a[2]);
      else
         r = nir_umin(&c->b, nir_umax(&c->b, a[0], a[1]), a[2]);
   } else if (!strcmp(op, "fast_ldexp") || !strcmp(op, "ldexp")) {
      NEED(2);
      r = nir_ldexp(&c->b, a[0], nir_i2i32(&c->b, a[1]));
   } else if (!strcmp(op, "fast_fma") || !strcmp(op, "fma") || !strcmp(op, "mad")) {
      NEED(3);
      r = nir_ffma(&c->b, a[0], a[1], a[2]);
   } else if (!strcmp(op, "abs")) {
      NEED(1);
      r = is_float ? nir_fabs(&c->b, a[0]) : nir_iabs(&c->b, a[0]);
   } else if (!strcmp(op, "abs_diff")) {
      NEED(2);
      r = is_signed ? nir_iabs(&c->b, nir_isub(&c->b, a[0], a[1]))
                    : nir_isub(&c->b, nir_umax(&c->b, a[0], a[1]),
                               nir_umin(&c->b, a[0], a[1]));
   } else if (!strcmp(op, "add_sat")) {
      NEED(2);
      r = is_signed ? nir_iadd_sat(&c->b, a[0], a[1]) : nir_uadd_sat(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "sub_sat")) {
      NEED(2);
      r = is_signed ? nir_isub_sat(&c->b, a[0], a[1]) : nir_usub_sat(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "mul_hi")) {
      NEED(2);
      r = is_signed ? nir_imul_high(&c->b, a[0], a[1]) : nir_umul_high(&c->b, a[0], a[1]);
   } else if (!strcmp(op, "popcount")) {
      NEED(1);
      r = nir_bit_count(&c->b, a[0]);
   } else if (!strcmp(op, "clz")) {
      NEED(1);
      r = nir_uclz(&c->b, a[0]);
   } else if (!strcmp(op, "ctz")) {
      NEED(1);
      /* Metal's ctz returns the type's width when the value is zero */
      nir_def *lsb = nir_find_lsb(&c->b, a[0]);
      r = nir_bcsel(&c->b, nir_ieq_imm(&c->b, a[0], 0),
                    nir_imm_intN_t(&c->b, a[0]->bit_size, a[0]->bit_size), lsb);
   } else if (!strcmp(op, "reverse_bits")) {
      NEED(1);
      r = nir_bitfield_reverse(&c->b, a[0]);
   } else if (!strcmp(op, "rotate")) {
      NEED(2);
      r = nir_urol(&c->b, a[0], nir_u2u32(&c->b, a[1]));
   } else if (!strcmp(op, "extract_bits")) {
      NEED(3);
      r = is_signed ? nir_ibitfield_extract(&c->b, a[0], nir_u2u32(&c->b, a[1]),
                                            nir_u2u32(&c->b, a[2]))
                    : nir_ubitfield_extract(&c->b, a[0], nir_u2u32(&c->b, a[1]),
                                            nir_u2u32(&c->b, a[2]));
   } else if (!strcmp(op, "insert_bits")) {
      NEED(4);
      r = nir_bitfield_insert(&c->b, a[0], a[1], nir_u2u32(&c->b, a[2]),
                              nir_u2u32(&c->b, a[3]));
   } else if (!strcmp(op, "dfdx")) {
      NEED(1);
      r = nir_ddx(&c->b, a[0]);
   } else if (!strcmp(op, "dfdy")) {
      NEED(1);
      r = nir_ddy(&c->b, a[0]);
   } else if (!strcmp(op, "fwidth")) {
      NEED(1);
      r = nir_fadd(&c->b, nir_fabs(&c->b, nir_ddx(&c->b, a[0])),
                   nir_fabs(&c->b, nir_ddy(&c->b, a[0])));
   } else if (!strcmp(op, "all")) {
      NEED(1);
      r = nir_ball(&c->b, a[0]);
   } else if (!strcmp(op, "any")) {
      NEED(1);
      r = nir_bany(&c->b, a[0]);
   } else if (!strcmp(op, "normalize_function_constant_predicate")) {
      NEED(1);
      r = nir_b2iN(&c->b, nir_ine_imm(&c->b, a[0], 0),
                   air_nir_bit_size(c, in->base.type));
   } else if (!strcmp(op, "unpack") && n->num >= 2 &&
              !strcmp(n->part[1], "unorm4x8")) {
      NEED(1);
      r = nir_unpack_unorm_4x8(&c->b, a[0]);
      if (air_nir_bit_size(c, in->base.type) == 16)
         r = nir_f2f16(&c->b, r);
   } else if (!strcmp(op, "pack") && n->num >= 2 &&
              !strcmp(n->part[1], "unorm4x8")) {
      NEED(1);
      nir_def *x = a[0]->bit_size == 16 ? nir_f2f32(&c->b, a[0]) : a[0];
      r = nir_pack_unorm_4x8(&c->b, x);
   } else if (!strcmp(op, "convert")) {
      NEED(1);
      /* air.convert.<dst sign>.<dst type>.<src sign>.<src type> */
      if (n->num < 5) {
         c->b.exact = was_exact;
         return air_error(c, "intrinsic: %s (a convert without both types)", op);
      }
      const struct air_type *dt = in->base.type;
      const struct air_type *st = in->operands[1]->type;
      unsigned bits = air_nir_bit_size(c, dt);
      bool dst_float = n->part[1][0] == 'f';
      bool src_float = n->part[3][0] == 'f';
      bool src_signed = n->part[3][0] == 's';
      nir_def *x = a[0];
      if (!src_float && air_type_bit_size(air_type_scalar(st)) == 1)
         x = src_signed ? nir_ineg(&c->b, nir_b2i32(&c->b, x)) : nir_b2i32(&c->b, x);
      if (bits == 1)
         r = src_float ? nir_fneu_imm(&c->b, x, 0.0) : nir_ine_imm(&c->b, x, 0);
      else if (dst_float && src_float)
         r = nir_f2fN(&c->b, x, bits);
      else if (dst_float)
         r = src_signed ? nir_i2fN(&c->b, x, bits) : nir_u2fN(&c->b, x, bits);
      else if (src_float)
         r = n->part[1][0] == 's' ? nir_f2iN(&c->b, x, bits) : nir_f2uN(&c->b, x, bits);
      else
         r = src_signed ? nir_i2iN(&c->b, x, bits) : nir_u2uN(&c->b, x, bits);
   } else {
      *handled = false;
   }
#undef NEED

   c->b.exact = was_exact;
   if (!*handled)
      return true;
   if (!r)
      return false;
   air_set(c, &in->base, air_val_ssa(c, r));
   return true;
}

/* -- function constants ------------------------------------------------- */

/* air.is_function_constant_defined(ptr to the constant's global)
 *
 * Metal's companion to `[[function_constant(n)]]`: a shader may ask whether
 * the pipeline supplied a value for a constant, and guard the code that reads
 * it. The answer is known at translation time -- it is exactly whether the
 * caller passed a value for that index in air_to_nir_options -- so this folds
 * to a constant and the guarded branch folds away with it.
 *
 * It is handled here, before math_op, because its argument is a POINTER and
 * math_op resolves every argument as a number: the four fixed_vert_* modules
 * stopped at "a pointer where a number belongs" with no instruction named,
 * because the failure was in reading the arguments and not in the operation.
 */
static bool
fc_defined_op(struct air_ctx *c, const struct air_inst *in)
{
   if (in->num_operands != 2)
      return air_error(c, "intrinsic: air.is_function_constant_defined of %u arguments",
                       in->num_operands - 1);
   struct air_val *p = air_get(c, in->operands[1]);
   if (!p)
      return false;
   if (p->kind != AIR_VAL_PTR || p->ptr.class != AIR_PTR_FUNCTION_CONSTANT)
      return air_error(c, "intrinsic: air.is_function_constant_defined of something "
                          "that is not a function constant");
   bool defined = false;
   for (unsigned i = 0; c->options && i < c->options->num_constants; i++)
      if (c->options->constants[i].index == p->ptr.binding &&
          c->options->constants[i].supplied)
         defined = true;
   air_set(c, &in->base, air_val_ssa(c, nir_imm_bool(&c->b, defined)));
   return true;
}

/* air.get_null_texture_* / air.get_null_depth_*: one reserved texture slot
 * allocated past the module's declared textures, marked null_texture so the
 * runtime binds its 1x1 zero texture there.
 */
static bool
null_tex_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n)
{
   if (in->num_operands != 1)
      return air_error(c, "intrinsic: air.%s of %u arguments",
                       n->part[0], in->num_operands - 1);
   if (!c->has_null_texture) {
      unsigned next = 0;
      for (unsigned i = 0; i < c->info->num_resources; i++) {
         const struct air_resource *r = &c->info->resources[i];
         if (r->kind == AIR_RESOURCE_TEXTURE || r->kind == AIR_RESOURCE_IMAGE)
            next = MAX2(next, r->index + 1);
      }
      struct air_shader_info *info = c->info;
      info->resources = reralloc(c->mem_ctx, info->resources, struct air_resource,
                                 info->num_resources + 1);
      info->resources[info->num_resources++] = (struct air_resource) {
         .kind = AIR_RESOURCE_TEXTURE, .index = next, .read = true,
         .param = ~0u, .name = "null_texture",
         .null_texture = true,
         .raster_order_group = -1,
         .argbuf_set = ~0u, .argbuf_id = ~0u,
      };
      c->has_null_texture = true;
      c->null_texture_slot = next;
   }
   struct air_ptr p = { .class = AIR_PTR_TEXTURE, .binding = c->null_texture_slot };
   air_set(c, &in->base, air_val_ptr(c, p));
   return true;
}

/* -- atomics ------------------------------------------------------------ */

static void
emit_atomic_order_barrier(struct air_ctx *c, uint64_t order, bool shared, bool after)
{
   /* Metal / C++14 memory_order:
    *   0 = relaxed, 1 = consume, 2 = acquire, 3 = release, 4 = acq_rel, 5 = seq_cst
    */
   if (order == 0)
      return;
   nir_memory_semantics sem = 0;
   if (!after && (order == 3 || order == 4 || order >= 5))
      sem = NIR_MEMORY_RELEASE;
   else if (after && (order == 1 || order == 2 || order == 4 || order >= 5))
      sem = NIR_MEMORY_ACQUIRE;
   if (!sem)
      return;

   nir_variable_mode modes = shared ? nir_var_mem_shared : nir_var_mem_ssbo;
   mesa_scope mscope = shared ? SCOPE_WORKGROUP : SCOPE_DEVICE;
   nir_barrier(&c->b, .execution_scope = SCOPE_NONE,
               .memory_scope = mscope, .memory_semantics = sem,
               .memory_modes = modes);
}

static bool
atomic_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n)
{
   /* air.atomic.fence(i32 flags, i32 order, i32 scope) */
   if (n->num == 2 && !strcmp(n->part[1], "fence")) {
      uint64_t flags = 3, order = 5, scope = 2;
      if (in->num_operands > 1)
         air_const_uint(in->operands[1], &flags);
      if (in->num_operands > 2)
         air_const_uint(in->operands[2], &order);
      if (in->num_operands > 3)
         air_const_uint(in->operands[3], &scope);
      if (order == 0)
         return true;
      nir_variable_mode modes = 0;
      if (flags & 1)
         modes |= nir_var_mem_ssbo | nir_var_mem_global;
      if (flags & 2)
         modes |= nir_var_mem_shared;
      if (flags & 4)
         modes |= nir_var_image;
      if (!modes)
         return true;
      mesa_scope mscope = scope == 1 ? SCOPE_WORKGROUP :
                          scope == 4 ? SCOPE_SUBGROUP : SCOPE_DEVICE;
      nir_barrier(&c->b, .execution_scope = SCOPE_NONE,
                  .memory_scope = mscope, .memory_semantics = NIR_MEMORY_ACQ_REL,
                  .memory_modes = modes);
      return true;
   }

   /* air.atomic.{global,local}.<op>[.weak|.strong][.<sign>].<type>(...) */
   if (n->num < 3)
      return air_error(c, "intrinsic: an atomic without a name");
   if (in->num_operands < 2)
      return air_error(c, "intrinsic: air.atomic without a pointer");
   const char *where = n->part[1];
   const char *op = n->part[2];
   if (strcmp(where, "global") && strcmp(where, "local"))
      return air_error(c, "intrinsic: air.atomic.%s", where);

   bool is_signed = true;
   for (unsigned i = 3; i < n->num; i++) {
      if (!strcmp(n->part[i], "s")) {
         is_signed = true;
         break;
      }
      if (!strcmp(n->part[i], "u")) {
         is_signed = false;
         break;
      }
   }

   struct air_val *pv = air_get(c, in->operands[1]);
   if (!pv)
      return false;
   if (pv->kind != AIR_VAL_PTR)
      return air_error(c, "intrinsic: an atomic on something that is not a pointer");
   struct air_ptr p = pv->ptr;

   bool shared = !strcmp(where, "local");
   if (shared != (p.class == AIR_PTR_SHARED))
      return air_error(c, "intrinsic: air.atomic.%s on %s memory", where,
                       p.class == AIR_PTR_SHARED ? "threadgroup" : "device");

   nir_def *off = p.offset ? p.offset : nir_imm_int(&c->b, 0);

   if (!strcmp(op, "cmpxchg")) {
      /* air.atomic.{global,local}.cmpxchg[.weak|.strong][.<sign>].<type>
       *   (ptr object, ptr|T expected, T desired, [i32 succ_order, i32 fail_order,
       *    i32 scope, i1 is_volatile])
       */
      if (in->num_operands < 4)
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg of %u arguments",
                          where, in->num_operands - 1);
      uint64_t order = 0;
      if (in->num_operands > 4 && !air_const_uint(in->operands[4], &order))
         order = 5;

      struct air_val *ev = air_get(c, in->operands[2]);
      nir_def *desired = air_get_ssa(c, in->operands[3]);
      if (!ev || !desired)
         return false;
      if (desired->num_components != 1)
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg of %u components",
                          where, desired->num_components);

      const struct air_type *val_type = in->operands[3]->type;
      nir_def *cmp = NULL;
      if (ev->kind == AIR_VAL_PTR) {
         cmp = air_load_ptr(c, ev->ptr, val_type, 0);
         if (!cmp)
            return false;
      } else if (ev->kind == AIR_VAL_SSA) {
         cmp = ev->def;
      } else {
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg with an unsupported "
                             "expected operand", where);
      }

      if (cmp->num_components != 1)
         return air_error(c, "intrinsic: air.atomic.%s.cmpxchg comparator has %u "
                             "components", where, cmp->num_components);
      if (cmp->bit_size != desired->bit_size)
         cmp = is_signed ? nir_i2iN(&c->b, cmp, desired->bit_size)
                         : nir_u2uN(&c->b, cmp, desired->bit_size);
      if (desired->bit_size != 32 && (shared || desired->bit_size != 64))
         return air_error(c, "intrinsic: %u-bit air.atomic.%s.cmpxchg is not supported",
                          desired->bit_size, where);

      emit_atomic_order_barrier(c, order, shared, false);

      nir_def *old;
      if (shared) {
         old = nir_shared_atomic_swap(&c->b, desired->bit_size, off, cmp, desired,
                                      .atomic_op = nir_atomic_op_cmpxchg);
      } else if (p.class == AIR_PTR_SSBO) {
         air_mark_written(c, AIR_RESOURCE_DEVICE_BUFFER, p.binding);
         old = nir_ssbo_atomic_swap(&c->b, desired->bit_size,
                                    nir_imm_int(&c->b, p.binding), off, cmp, desired,
                                    .atomic_op = nir_atomic_op_cmpxchg);
      } else {
         return air_error(c, "intrinsic: an atomic on a pointer that is not a buffer");
      }

      emit_atomic_order_barrier(c, order, shared, true);

      /* C11 / Metal atomic_compare_exchange updates *expected with the old
       * value on failure; on success old == cmp so storing old unconditionally
       * writes the identical value without introducing control flow.
       */
      if (ev->kind == AIR_VAL_PTR) {
         if (!air_store_ptr(c, ev->ptr, old, 0))
            return false;
      }

      nir_def *ok = nir_ieq(&c->b, old, cmp);
      const struct air_type *rt = in->base.type;
      if (!rt || rt->kind == AIR_TYPE_VOID)
         return true;
      if (rt->kind == AIR_TYPE_STRUCT) {
         struct air_val *agg = rzalloc(c->mem_ctx, struct air_val);
         agg->kind = AIR_VAL_AGG;
         agg->num_elems = rt->num_members;
         agg->elems = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(rt->num_members, 1));
         if (rt->num_members >= 1)
            agg->elems[0] = air_val_ssa(c, old);
         if (rt->num_members >= 2) {
            unsigned sbits = air_nir_bit_size(c, rt->members[1]);
            agg->elems[1] = air_val_ssa(c, sbits == 1 ? ok : nir_b2iN(&c->b, ok, sbits));
         }
         for (unsigned i = 2; i < rt->num_members; i++) {
            unsigned mbits = air_nir_bit_size(c, rt->members[i]);
            agg->elems[i] = air_val_ssa(c, nir_imm_intN_t(&c->b, 0, mbits));
         }
         air_set(c, &in->base, agg);
         return true;
      }
      unsigned rbits = air_nir_bit_size(c, rt);
      if (rt->kind == AIR_TYPE_INT && rbits == 1)
         air_set(c, &in->base, air_val_ssa(c, ok));
      else if (rt->kind == AIR_TYPE_INT && rbits == 8 && old->bit_size != 8)
         air_set(c, &in->base, air_val_ssa(c, nir_b2i8(&c->b, ok)));
      else
         air_set(c, &in->base, air_val_ssa(c, old));
      return true;
   }

   unsigned iorder = !strcmp(op, "load") ? 2 : 3;
   uint64_t order = 0;
   if (in->num_operands > iorder && !air_const_uint(in->operands[iorder], &order))
      order = 5;

   if (!strcmp(op, "load")) {
      nir_def *r = air_load_ptr(c, p, in->base.type, 0);
      if (!r)
         return false;
      emit_atomic_order_barrier(c, order, shared, true);
      air_set(c, &in->base, air_val_ssa(c, r));
      return true;
   }
   if (!strcmp(op, "store")) {
      if (in->num_operands <= 2)
         return air_error(c, "intrinsic: air.atomic.%s.store without a value", where);
      nir_def *v = air_get_ssa(c, in->operands[2]);
      if (!v)
         return false;
      emit_atomic_order_barrier(c, order, shared, false);
      if (!air_store_ptr(c, p, v, 0))
         return false;
      if (order >= 5)
         emit_atomic_order_barrier(c, order, shared, true);
      return true;
   }

   if (in->num_operands <= 2)
      return air_error(c, "intrinsic: air.atomic.%s.%s without a value", where, op);

   nir_def *v = air_get_ssa(c, in->operands[2]);
   if (!v)
      return false;

   nir_atomic_op aop;
   if (!strcmp(op, "add")) {
      aop = nir_atomic_op_iadd;
   } else if (!strcmp(op, "sub")) {
      aop = nir_atomic_op_iadd;
      v = nir_ineg(&c->b, v);
   } else if (!strcmp(op, "min")) {
      aop = is_signed ? nir_atomic_op_imin : nir_atomic_op_umin;
   } else if (!strcmp(op, "max")) {
      aop = is_signed ? nir_atomic_op_imax : nir_atomic_op_umax;
   } else if (!strcmp(op, "and")) {
      aop = nir_atomic_op_iand;
   } else if (!strcmp(op, "or")) {
      aop = nir_atomic_op_ior;
   } else if (!strcmp(op, "xor")) {
      aop = nir_atomic_op_ixor;
   } else if (!strcmp(op, "xchg")) {
      aop = nir_atomic_op_xchg;
   } else {
      return air_error(c, "intrinsic: atomic %s", op);
   }

   emit_atomic_order_barrier(c, order, shared, false);

   nir_def *r;
   if (shared) {
      r = nir_shared_atomic(&c->b, v->bit_size, off, v, .atomic_op = aop);
   } else if (p.class == AIR_PTR_SSBO) {
      air_mark_written(c, AIR_RESOURCE_DEVICE_BUFFER, p.binding);
      r = nir_ssbo_atomic(&c->b, v->bit_size, nir_imm_int(&c->b, p.binding), off, v,
                          .atomic_op = aop);
   } else {
      return air_error(c, "intrinsic: an atomic on a pointer that is not a buffer");
   }

   emit_atomic_order_barrier(c, order, shared, true);

   if (in->base.type && in->base.type->kind != AIR_TYPE_VOID)
      air_set(c, &in->base, air_val_ssa(c, r));
   return true;
}

/* -- barriers ----------------------------------------------------------- */

static bool
barrier_op(struct air_ctx *c, const struct air_inst *in, bool simdgroup)
{
   /* air.{wg,simdgroup}.barrier(i32 flags, i32 scope): the flags say which
    * memory is ordered (0 none, 1 device, 2 threadgroup, 4 texture).
    */
   uint64_t flags = simdgroup ? 0 : 2;
   bool have_flags = in->num_operands > 1 && air_const_uint(in->operands[1], &flags);
   mesa_scope escope = simdgroup ? SCOPE_SUBGROUP : SCOPE_WORKGROUP;

   nir_variable_mode modes = 0;
   if (flags & 1)
      modes |= nir_var_mem_ssbo | nir_var_mem_global;
   if (flags & 2)
      modes |= nir_var_mem_shared;
   if (flags & 4)
      modes |= nir_var_image;
   if (!modes && !have_flags && !simdgroup)
      modes = nir_var_mem_shared;

   if (modes) {
      nir_barrier(&c->b, .execution_scope = escope,
                  .memory_scope = escope, .memory_semantics = NIR_MEMORY_ACQ_REL,
                  .memory_modes = modes);
   } else {
      nir_barrier(&c->b, .execution_scope = escope);
   }
   return true;
}

/* -- fragment flow control ---------------------------------------------- */

/* air.discard_fragment()
 *
 * NIR'S `demote` AND NOT NIR'S `terminate`, and that choice is the whole of
 * this function. The two differ in what the thread does afterwards: terminate
 * ends the invocation, so the lane stops contributing to its quad's
 * derivatives and performs no further memory writes, while demote turns it
 * into a HELPER invocation -- it keeps executing, its framebuffer and memory
 * writes are dropped, and it still supplies the neighbour values that a
 * derivative or an implicit-level-of-detail sample in the OTHER three lanes
 * of the quad needs.
 *
 * It is a real distinction in NIR and in every backend that has one: ir3
 * keeps the two apart in its own IR (ir3_compiler_nir.c emits ir3_DEMOTE
 * against ir3_KILL, and they carry different scheduling barriers), and
 * nir_to_tgsi sends them to TGSI's DEMOTE and KILL, which are separate
 * opcodes. ONE PLACE COLLAPSES THEM, AND IT IS WORTH KNOWING ABOUT: the a6xx
 * instruction encoder maps DEMOTE onto the hardware's KILL --
 * `} else if (instr->opc == OPC_DEMOTE) { return OPC_KILL; }` in
 * src/freedreno/isa/encode.c -- and the ISA description has no demote bitset
 * at all (ir3-cat0.xml carries `kill` and nothing beside it). So on this GPU,
 * with this Mesa, both come out as one instruction and the helper lanes Metal
 * promises are not delivered by the hardware path today. That is a gap to
 * name, not a reason to translate this as `terminate`: terminate would lose
 * the same lanes and would be wrong on a backend that does tell them apart.
 * What it costs is measurable only on the phone -- a shader that discards and
 * then takes a derivative -- and that is written as a job rather than assumed.
 *
 * ONE THING IS NOT A DIFFERENCE BETWEEN THE TWO, and saying so is the
 * correction of a sentence that used to stand here: stores. NIR's own
 * definition of demote (nir_intrinsics.py) is "Any stores to memory after
 * this instruction are suppressed and the fragment does not write outputs to
 * the framebuffer. Unlike discard, demote needs to ensure that derivatives
 * will still work for invocations that were not demoted." So demote drops the
 * lane's later writes as well -- it differs from terminate in the derivatives
 * and in the lane going on executing, not in the memory. Whether METAL drops
 * them is a third question, and nothing on this machine answers it: no header
 * on the device says, and a corpus cannot show it. If Metal keeps them, both
 * of NIR's intrinsics are wrong about it by the same amount.
 *
 * Nothing in a corpus COUNT can decide it -- both translate, both validate,
 * both compile -- so it is decided from two independent readings of what
 * Metal means, and both say the thread lives:
 *
 *  1. The shaders themselves. Resident Evil 4's `PS_DrawByTiles` calls
 *     air.discard_fragment at the ninth instruction of a 1 800-line function
 *     and then calls `air.simd_is_helper_thread()` EIGHT TIMES further down
 *     it. A terminated thread cannot be asked whether it is a helper thread;
 *     a demoted one answers yes, which is the only reading under which that
 *     shader means anything. 24 of the library's 1 288 discard-calling modules
 *     call that function.
 *  2. Apple's own declaration of it. `llvm-dis` of that module reads
 *     `declare void @air.discard_fragment() local_unnamed_addr #8` with
 *     `attributes #8 = { nounwind }` -- and NOT `noreturn`. LLVM is entitled
 *     to delete everything after a call it is told does not return, and
 *     Apple's compiler kept the rest of the function; the call is followed by
 *     `br` and not `unreachable` in 73 of 85 call sites sampled. A discard
 *     that killed the thread would be a noreturn call.
 *
 * And the cost of getting it wrong is not hypothetical here: of 85 sampled
 * call sites, 45 are followed in the same function by an air.sample_* with an
 * implicit level of detail and 6 by air.dfdx, both of which read the other
 * lanes of the quad. Under `terminate` those neighbours would read a dead
 * lane.
 *
 * uses_discard is set by hand because nir_shader_gather_info does not run on
 * this path (ir3 calls it only for a binning variant), exactly as the
 * framebuffer-fetch flags are set in air_to_nir.c. It is not bookkeeping: it
 * is what nir_to_tgsi_info turns into `uses_kill`, which is what a Gallium
 * driver reads before it allows an early depth test, and what freedreno reads
 * before it writes LRZ. A discarding shader whose interface says it discards
 * nothing gets its depth written for pixels it threw away.
 */
static bool
discard_op(struct air_ctx *c, const struct air_inst *in)
{
   if (c->nir->info.stage != MESA_SHADER_FRAGMENT)
      return air_error(c, "intrinsic: air.discard_fragment in a %s shader",
                       gl_shader_stage_name(c->nir->info.stage));
   nir_demote(&c->b);
   c->nir->info.fs.uses_discard = true;
   return true;
}

/* -- SIMD groups and quads ----------------------------------------------- */

/* air.simd_* and air.quad_*: Metal's cross-lane operations.
 *
 * THE SIMD-GROUP HERE IS THE IMPLEMENTATION'S, NOT A 32-LANE ONE, and that is
 * the only way these can be translated honestly. Metal does not fix the width
 * -- a shader asks for it with air.get_simd_width (this file answers with
 * nir_load_subgroup_size) and a host asks MTLComputePipelineState for
 * threadExecutionWidth -- and it is 32 on every Apple GPU, which is why a
 * shader compiled for one may have the number built into it. This hardware's
 * is 64 or 128: `threadsize_base` is 64 for the A635 this port targets and ir3
 * doubles it per variant in the compute and fragment stages
 * (ir3_shader_get_subgroup_size, IR3_SINGLE_OR_DOUBLE).
 *
 * So the width a translated shader is TOLD and the width its votes are taken
 * over are the same number, whatever that number is: a shader that reads
 * air.get_simd_width and works from it is right, and a shader with 32 written
 * into it is wrong -- as it would be on any Metal implementation whose
 * SIMD-group is not 32 wide. Each of these leaves a note saying so, because it
 * is a thing the hardware cannot do exactly rather than a thing this frontend
 * chose.
 *
 * One more thing decides whether that consistency holds, and it is a property
 * of the GPU and not of this file: ir3 declares a fragment or vertex
 * subgroup to be ONE LANE and lowers a vote to its own argument on a device
 * without `has_getfiberid` (ir3_nir.c: `options.subgroup_size = 1`,
 * `lower_vote_trivial`). The A635 has it -- it is an a6xx gen4, and
 * freedreno_devices.py sets the flag for gen4 and above -- so the votes here
 * are real in every stage, and air.get_simd_width answers with the real width
 * in the same stages. On an a6xx below gen4 both would read 1, which is still
 * self-consistent and still not Metal's 32.
 *
 * air.simd_ballot is the one that cannot be translated at all, and it is
 * refused by name below rather than noted: its result is a 64-bit mask with
 * one bit per lane, and a lane count this frontend cannot bound at 64. A
 * popcount of a truncated mask is a smaller number and not a worse one.
 *
 * The QUAD operations are a different matter and are exact: a quad is four
 * consecutive lanes on both machines -- it is what a fragment derivative is
 * computed over -- so a swap inside one has the same meaning whatever the
 * SIMD-group's width is, and nir_lower_subgroups leaves the quad intrinsics
 * alone for ir3 (no lower_quad, and lower_vote_trivial does not reach them).
 */
enum air_lane_move {
   AIR_MOVE_QUAD_BRCST,
   AIR_MOVE_QUAD_HORIZ,
   AIR_MOVE_QUAD_VERT,
   AIR_MOVE_QUAD_DIAG,
   AIR_MOVE_SIMD_FIRST,
   AIR_MOVE_SIMD_SHUFFLE,
   AIR_MOVE_SIMD_XOR,
   AIR_MOVE_SIMD_UP,
   AIR_MOVE_SIMD_DOWN,
};

enum air_scan_kind {
   AIR_SCAN_REDUCE,
   AIR_SCAN_INCLUSIVE,
   AIR_SCAN_EXCLUSIVE,
};

static nir_def *
fit_bool_result(struct air_ctx *c, const struct air_type *rt, nir_def *cond)
{
   unsigned bits = air_nir_bit_size(c, rt);
   return bits == 1 ? cond : nir_b2iN(&c->b, cond, bits);
}

static nir_def *
air_quad_lane_id(struct air_ctx *c)
{
   if (c->nir->info.stage == MESA_SHADER_COMPUTE)
      return nir_iand_imm(&c->b, nir_load_local_invocation_index(&c->b), 3);
   if (c->nir->info.stage == MESA_SHADER_FRAGMENT) {
      nir_def *fc = air_frag_coord(c);
      nir_def *qx = nir_iand_imm(&c->b, nir_f2u32(&c->b, nir_channel(&c->b, fc, 0)), 1);
      nir_def *qy = nir_iand_imm(&c->b, nir_f2u32(&c->b, nir_channel(&c->b, fc, 1)), 1);
      return nir_ior(&c->b, qx, nir_ishl_imm(&c->b, qy, 1));
   }
   return nir_imm_int(&c->b, 0);
}

static nir_def *
lane_move_16_32(struct air_ctx *c, nir_def *s, nir_def *idx, enum air_lane_move kind)
{
   switch (kind) {
   case AIR_MOVE_QUAD_BRCST:  return nir_quad_broadcast(&c->b, s, idx);
   case AIR_MOVE_QUAD_HORIZ:  return nir_quad_swap_horizontal(&c->b, s);
   case AIR_MOVE_QUAD_VERT:   return nir_quad_swap_vertical(&c->b, s);
   case AIR_MOVE_QUAD_DIAG:   return nir_quad_swap_diagonal(&c->b, s);
   case AIR_MOVE_SIMD_FIRST:  return nir_read_first_invocation(&c->b, s);
   case AIR_MOVE_SIMD_SHUFFLE: return nir_shuffle(&c->b, s, idx);
   case AIR_MOVE_SIMD_XOR:    return nir_shuffle_xor(&c->b, s, idx);
   case AIR_MOVE_SIMD_UP:     return nir_shuffle_up(&c->b, s, idx);
   case AIR_MOVE_SIMD_DOWN:   return nir_shuffle_down(&c->b, s, idx);
   }
   return s;
}

/* ir3's quad_shuffle and shfl instructions operate on 16-bit and 32-bit scalar
 * registers (ir3_nir.c does not enable lower_boolean_shuffle or
 * lower_shuffle_to_32bit). Widen 1/8-bit scalars and split 64-bit scalars into
 * 2x32 halves here, scalarizing vectors component-by-component.
 */
static nir_def *
lane_move_scalar(struct air_ctx *c, nir_def *s, nir_def *idx, enum air_lane_move kind)
{
   if (s->bit_size == 1) {
      nir_def *r = lane_move_16_32(c, nir_b2i32(&c->b, s), idx, kind);
      return nir_ine_imm(&c->b, r, 0);
   }
   if (s->bit_size == 8) {
      nir_def *r = lane_move_16_32(c, nir_u2u16(&c->b, s), idx, kind);
      return nir_u2u8(&c->b, r);
   }
   if (s->bit_size == 64) {
      nir_def *lo = lane_move_16_32(c, nir_unpack_64_2x32_split_x(&c->b, s), idx, kind);
      nir_def *hi = lane_move_16_32(c, nir_unpack_64_2x32_split_y(&c->b, s), idx, kind);
      return nir_pack_64_2x32_split(&c->b, lo, hi);
   }
   return lane_move_16_32(c, s, idx, kind);
}

static nir_def *
lane_move_vec(struct air_ctx *c, nir_def *val, nir_def *idx, enum air_lane_move kind)
{
   if (val->num_components == 1)
      return lane_move_scalar(c, val, idx, kind);
   nir_def *comps[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < val->num_components; i++)
      comps[i] = lane_move_scalar(c, nir_channel(&c->b, val, i), idx, kind);
   return nir_vec(&c->b, comps, val->num_components);
}

/* Arbitrary per-lane 4-element quad permutation using four constant-lane
 * quad_broadcast ops, avoiding GETFIBERID and divergent-index quirks on a610.
 */
static nir_def *
quad_select_vec(struct air_ctx *c, nir_def *val, nir_def *lane32)
{
   nir_def *v0 = lane_move_vec(c, val, nir_imm_int(&c->b, 0), AIR_MOVE_QUAD_BRCST);
   nir_def *v1 = lane_move_vec(c, val, nir_imm_int(&c->b, 1), AIR_MOVE_QUAD_BRCST);
   nir_def *v2 = lane_move_vec(c, val, nir_imm_int(&c->b, 2), AIR_MOVE_QUAD_BRCST);
   nir_def *v3 = lane_move_vec(c, val, nir_imm_int(&c->b, 3), AIR_MOVE_QUAD_BRCST);
   nir_def *b0 = nir_ine_imm(&c->b, nir_iand_imm(&c->b, lane32, 1), 0);
   nir_def *b1 = nir_ine_imm(&c->b, nir_iand_imm(&c->b, lane32, 2), 0);
   nir_def *lo = nir_bcsel(&c->b, b0, v1, v0);
   nir_def *hi = nir_bcsel(&c->b, b0, v3, v2);
   return nir_bcsel(&c->b, b1, hi, lo);
}

static nir_def *
reduce_identity_scalar(struct air_ctx *c, nir_op rop, unsigned bits)
{
   switch (rop) {
   case nir_op_fadd:
      return nir_imm_floatN_t(&c->b, 0.0, bits);
   case nir_op_fmul:
      return nir_imm_floatN_t(&c->b, 1.0, bits);
   case nir_op_fmin:
      return nir_imm_floatN_t(&c->b, INFINITY, bits);
   case nir_op_fmax:
      return nir_imm_floatN_t(&c->b, -INFINITY, bits);
   case nir_op_iadd:
   case nir_op_ior:
   case nir_op_ixor:
   case nir_op_umax:
      return nir_imm_intN_t(&c->b, 0, bits);
   case nir_op_imul:
      return nir_imm_intN_t(&c->b, 1, bits);
   case nir_op_iand:
   case nir_op_umin:
      return nir_imm_intN_t(&c->b, -1, bits);
   case nir_op_imin:
      return nir_imm_intN_t(&c->b,
                            bits == 64 ? INT64_MAX
                                       : (int64_t)((1ull << (bits - 1)) - 1),
                            bits);
   case nir_op_imax:
      return nir_imm_intN_t(&c->b,
                            bits == 64 ? INT64_MIN
                                       : -(int64_t)(1ull << (bits - 1)),
                            bits);
   default:
      return nir_imm_intN_t(&c->b, 0, bits);
   }
}

static nir_def *
reduce_identity_vec(struct air_ctx *c, nir_op rop, unsigned comps, unsigned bits)
{
   nir_def *s = reduce_identity_scalar(c, rop, bits);
   if (comps == 1)
      return s;
   nir_def *v[NIR_MAX_VEC_COMPONENTS];
   for (unsigned i = 0; i < comps; i++)
      v[i] = s;
   return nir_vec(&c->b, v, comps);
}

static nir_def *
emit_simd_scan_16_32(struct air_ctx *c, nir_def *s, nir_op rop, enum air_scan_kind skind)
{
   switch (skind) {
   case AIR_SCAN_REDUCE:
      return nir_reduce(&c->b, s, .reduction_op = rop);
   case AIR_SCAN_INCLUSIVE:
      return nir_inclusive_scan(&c->b, s, .reduction_op = rop);
   case AIR_SCAN_EXCLUSIVE:
      return nir_exclusive_scan(&c->b, s, .reduction_op = rop);
   }
   return s;
}

static nir_def *
emit_simd_scan_scalar(struct air_ctx *c, nir_def *s, nir_op rop, bool is_signed,
                      enum air_scan_kind skind, const char *opname)
{
   if (s->bit_size == 16 || s->bit_size == 32)
      return emit_simd_scan_16_32(c, s, rop, skind);

   if (s->bit_size == 1) {
      nir_def *w = (rop == nir_op_iand || rop == nir_op_umin)
                      ? nir_bcsel(&c->b, s, nir_imm_int(&c->b, -1), nir_imm_int(&c->b, 0))
                      : nir_b2i32(&c->b, s);
      nir_def *r = emit_simd_scan_16_32(c, w, rop, skind);
      return nir_ine_imm(&c->b, r, 0);
   }
   if (s->bit_size == 8) {
      nir_def *w = is_signed ? nir_i2i32(&c->b, s) : nir_u2u32(&c->b, s);
      nir_def *r = emit_simd_scan_16_32(c, w, rop, skind);
      return nir_u2u8(&c->b, r);
   }
   if (s->bit_size == 64) {
      if (rop == nir_op_iand || rop == nir_op_ior || rop == nir_op_ixor) {
         nir_def *lo = emit_simd_scan_16_32(c, nir_unpack_64_2x32_split_x(&c->b, s),
                                            rop, skind);
         nir_def *hi = emit_simd_scan_16_32(c, nir_unpack_64_2x32_split_y(&c->b, s),
                                            rop, skind);
         return nir_pack_64_2x32_split(&c->b, lo, hi);
      }
      air_note(c, "air.%s: 64-bit arithmetic subgroup reduction narrowed to 32-bit on a6xx",
               opname);
      nir_def *r32 = emit_simd_scan_16_32(c, nir_u2u32(&c->b, s), rop, skind);
      return is_signed ? nir_i2i64(&c->b, r32) : nir_u2u64(&c->b, r32);
   }
   return s;
}

static bool
parse_reduce_op(const char *s, bool is_float, bool is_signed,
                enum air_scan_kind *skind, nir_op *rop)
{
   if (!strncmp(s, "prefix_exclusive_", 17)) {
      *skind = AIR_SCAN_EXCLUSIVE;
      s += 17;
   } else if (!strncmp(s, "prefix_inclusive_", 17)) {
      *skind = AIR_SCAN_INCLUSIVE;
      s += 17;
   } else {
      *skind = AIR_SCAN_REDUCE;
   }

   if (!strcmp(s, "sum")) {
      *rop = is_float ? nir_op_fadd : nir_op_iadd;
      return true;
   }
   if (!strcmp(s, "product") || !strcmp(s, "mul")) {
      *rop = is_float ? nir_op_fmul : nir_op_imul;
      return true;
   }
   if (!strcmp(s, "min")) {
      *rop = is_float ? nir_op_fmin : (is_signed ? nir_op_imin : nir_op_umin);
      return true;
   }
   if (!strcmp(s, "max")) {
      *rop = is_float ? nir_op_fmax : (is_signed ? nir_op_imax : nir_op_umax);
      return true;
   }
   if (!strcmp(s, "and") && !is_float) {
      *rop = nir_op_iand;
      return true;
   }
   if (!strcmp(s, "or") && !is_float) {
      *rop = nir_op_ior;
      return true;
   }
   if (!strcmp(s, "xor") && !is_float) {
      *rop = nir_op_ixor;
      return true;
   }
   return false;
}

static bool
simd_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n,
        bool *handled)
{
   const char *op = n->part[0];

   /* THE NAME IS DECIDED BEFORE AN ARGUMENT IS TOUCHED, which is the same rule
    * math_op states above it and fc_defined_op exists for -- and getting it
    * wrong here cost 46 modules of the 456 for the length of one commit. This
    * function used to resolve its first argument before asking whether the
    * name was one of its own, and every texture operation passes a texture
    * HANDLE as that argument: air.sample_texture_2d came back "instruction: a
    * pointer where a number belongs", which reads like a broken module and
    * names no operation at all. The count was the only visible symptom and it
    * was briefly attributed to somebody else's commit.
    */
   if (strncmp(op, "simd_", 5) && strncmp(op, "quad_", 5) &&
       strcmp(op, "get_sample_position") && strcmp(op, "is_uniform") &&
       strcmp(op, "get_simd_width") && strcmp(op, "get_thread_execution_width") &&
       strcmp(op, "get_quad_width")) {
      *handled = false;
      return true;
   }
   *handled = true;

   bool is_cs = c->nir->info.stage == MESA_SHADER_COMPUTE;
   bool is_vs = c->nir->info.stage == MESA_SHADER_VERTEX;

   if (!strcmp(op, "simd_is_helper_thread")) {
      if (c->nir->info.stage != MESA_SHADER_FRAGMENT)
         return air_error(c, "intrinsic: air.simd_is_helper_thread in a %s shader "
                             "(there are no helper threads outside the fragment "
                             "stage)", gl_shader_stage_name(c->nir->info.stage));
      air_set(c, &in->base,
              air_val_ssa(c, fit_bool_result(c, in->base.type,
                                             nir_is_helper_invocation(&c->b, 1))));
      return true;
   }

   if (!strcmp(op, "get_simd_width") || !strcmp(op, "get_thread_execution_width")) {
      unsigned bits = air_nir_bit_size(c, in->base.type);
      nir_def *w = is_cs ? nir_load_subgroup_size(&c->b) : nir_imm_int(&c->b, 1);
      air_set(c, &in->base, air_val_ssa(c, nir_u2uN(&c->b, w, bits)));
      return true;
   }

   if (!strcmp(op, "get_quad_width")) {
      unsigned bits = air_nir_bit_size(c, in->base.type);
      air_set(c, &in->base, air_val_ssa(c, nir_imm_intN_t(&c->b, is_vs ? 1 : 4, bits)));
      return true;
   }

   if (!strcmp(op, "simd_is_first")) {
      nir_def *first = is_cs ? nir_elect(&c->b, 1) : nir_imm_true(&c->b);
      air_set(c, &in->base, air_val_ssa(c, fit_bool_result(c, in->base.type, first)));
      return true;
   }

   if (!strcmp(op, "quad_is_first")) {
      nir_def *first = is_vs ? nir_imm_true(&c->b)
                             : nir_ieq_imm(&c->b, air_quad_lane_id(c), 0);
      air_set(c, &in->base, air_val_ssa(c, fit_bool_result(c, in->base.type, first)));
      return true;
   }

   /* air.get_sample_position(index, 0): refused by name (see half-pixel note). */
   if (!strcmp(op, "get_sample_position"))
      return air_error(c, "intrinsic: air.%s -- NIR's sample position is an offset "
                          "from the pixel centre and Metal's convention is not "
                          "attested anywhere on this machine; the two differ by half "
                          "a pixel, so this is left to a measurement", n->part[0]);

   nir_def *a0 = in->num_operands > 1 ? air_get_ssa(c, in->operands[1]) : NULL;
   if (in->num_operands > 1 && !a0)
      return false;

   if (!strcmp(op, "simd_ballot")) {
      if (!a0)
         return air_error(c, "intrinsic: air.%s without an argument", n->part[0]);
      unsigned comps = air_nir_components(in->base.type);
      unsigned bits = air_nir_bit_size(c, in->base.type);
      if (comps * bits > 64)
         return air_error(c, "intrinsic: air.%s wider than 64 bits", n->part[0]);
      air_note(c, "air.%s produces a mask over the SIMD group this implementation has",
               n->part[0]);
      nir_def *cond = a0->bit_size == 1 ? a0 : nir_ine_imm(&c->b, a0, 0);
      nir_def *res;
      if (!is_cs) {
         res = nir_b2iN(&c->b, cond, bits);
         if (comps > 1)
            res = nir_pad_vector_imm_int(&c->b, res, 0, comps);
      } else if (bits < 32) {
         res = nir_u2uN(&c->b, nir_ballot(&c->b, 1, 32, cond), bits);
         if (comps > 1)
            res = nir_pad_vector_imm_int(&c->b, res, 0, comps);
      } else {
         res = nir_ballot(&c->b, comps, bits, cond);
      }
      air_set(c, &in->base, air_val_ssa(c, res));
      return true;
   }

   if (!strcmp(op, "quad_ballot")) {
      if (!a0)
         return air_error(c, "intrinsic: air.%s without an argument", n->part[0]);
      unsigned comps = air_nir_components(in->base.type);
      unsigned bits = air_nir_bit_size(c, in->base.type);
      nir_def *cond = a0->bit_size == 1 ? a0 : nir_ine_imm(&c->b, a0, 0);
      nir_def *b32 = nir_b2i32(&c->b, cond);
      nir_def *mask;
      if (is_vs) {
         mask = b32;
      } else {
         nir_def *v0 = nir_quad_broadcast(&c->b, b32, nir_imm_int(&c->b, 0));
         nir_def *v1 = nir_ishl_imm(&c->b,
                                    nir_quad_broadcast(&c->b, b32, nir_imm_int(&c->b, 1)), 1);
         nir_def *v2 = nir_ishl_imm(&c->b,
                                    nir_quad_broadcast(&c->b, b32, nir_imm_int(&c->b, 2)), 2);
         nir_def *v3 = nir_ishl_imm(&c->b,
                                    nir_quad_broadcast(&c->b, b32, nir_imm_int(&c->b, 3)), 3);
         mask = nir_ior(&c->b, nir_ior(&c->b, v0, v1), nir_ior(&c->b, v2, v3));
      }
      nir_def *res = nir_u2uN(&c->b, mask, bits);
      if (comps > 1)
         res = nir_pad_vector_imm_int(&c->b, res, 0, comps);
      air_set(c, &in->base, air_val_ssa(c, res));
      return true;
   }

   if (!strcmp(op, "is_uniform") || !strcmp(op, "simd_is_uniform")) {
      if (!a0)
         return air_error(c, "intrinsic: air.%s without an argument", n->part[0]);
      if (!is_cs) {
         air_set(c, &in->base,
                 air_val_ssa(c, fit_bool_result(c, in->base.type, nir_imm_true(&c->b))));
         return true;
      }
      bool is_f = air_type_is_float(air_type_scalar(in->operands[1]->type));
      nir_def *res = NULL;
      for (unsigned i = 0; i < a0->num_components; i++) {
         nir_def *ch = a0->num_components == 1 ? a0 : nir_channel(&c->b, a0, i);
         nir_def *eq;
         if (ch->bit_size == 64) {
            nir_def *lo = nir_vote_ieq(&c->b, 1, nir_unpack_64_2x32_split_x(&c->b, ch));
            nir_def *hi = nir_vote_ieq(&c->b, 1, nir_unpack_64_2x32_split_y(&c->b, ch));
            eq = nir_iand(&c->b, lo, hi);
         } else {
            if (ch->bit_size == 1)
               ch = nir_b2i32(&c->b, ch);
            else if (ch->bit_size == 8)
               ch = nir_u2u16(&c->b, ch);
            eq = is_f ? nir_vote_feq(&c->b, 1, ch) : nir_vote_ieq(&c->b, 1, ch);
         }
         res = res ? nir_iand(&c->b, res, eq) : eq;
      }
      air_set(c, &in->base, air_val_ssa(c, fit_bool_result(c, in->base.type, res)));
      return true;
   }

   if (!strcmp(op, "quad_is_uniform")) {
      if (!a0)
         return air_error(c, "intrinsic: air.%s without an argument", n->part[0]);
      if (is_vs) {
         air_set(c, &in->base,
                 air_val_ssa(c, fit_bool_result(c, in->base.type, nir_imm_true(&c->b))));
         return true;
      }
      bool is_f = air_type_is_float(air_type_scalar(in->operands[1]->type));
      nir_def *first = lane_move_vec(c, a0, nir_imm_int(&c->b, 0), AIR_MOVE_QUAD_BRCST);
      nir_def *same = is_f ? nir_ball_fequal(&c->b, a0, first)
                           : nir_ball_iequal(&c->b, a0, first);
      nir_def *u = nir_b2i32(&c->b, same);
      u = nir_iand(&c->b, u, nir_quad_swap_horizontal(&c->b, u));
      u = nir_iand(&c->b, u, nir_quad_swap_vertical(&c->b, u));
      air_set(c, &in->base,
              air_val_ssa(c, fit_bool_result(c, in->base.type, nir_ine_imm(&c->b, u, 0))));
      return true;
   }

   if (!strcmp(op, "simd_all") || !strcmp(op, "simd_any")) {
      if (!a0)
         return air_error(c, "intrinsic: air.%s without an argument", n->part[0]);
      bool all = !strcmp(op, "simd_all");
      nir_def *cond = a0->bit_size == 1 ? a0 : nir_ine_imm(&c->b, a0, 0);
      nir_def *res;
      if (!is_cs) {
         res = cond;
      } else {
         air_note(c, "air.%s is taken over the SIMD group this implementation has, "
                     "which is not Metal's 32 lanes: ir3 runs 64 or 128 of them on this "
                     "hardware. air.get_simd_width answers with the same number, so a "
                     "shader that reads it is right and one with 32 built into it is not",
                  n->part[0]);
         res = all ? nir_vote_all(&c->b, 1, cond) : nir_vote_any(&c->b, 1, cond);
      }
      air_set(c, &in->base, air_val_ssa(c, fit_bool_result(c, in->base.type, res)));
      return true;
   }

   if (!strcmp(op, "quad_all") || !strcmp(op, "quad_any")) {
      if (!a0)
         return air_error(c, "intrinsic: air.%s without an argument", n->part[0]);
      bool all = !strcmp(op, "quad_all");
      nir_def *cond = a0->bit_size == 1 ? a0 : nir_ine_imm(&c->b, a0, 0);
      if (is_vs) {
         air_set(c, &in->base, air_val_ssa(c, fit_bool_result(c, in->base.type, cond)));
         return true;
      }
      nir_def *u = nir_b2i32(&c->b, cond);
      nir_def *h = nir_quad_swap_horizontal(&c->b, u);
      u = all ? nir_iand(&c->b, u, h) : nir_ior(&c->b, u, h);
      nir_def *v = nir_quad_swap_vertical(&c->b, u);
      u = all ? nir_iand(&c->b, u, v) : nir_ior(&c->b, u, v);
      air_set(c, &in->base,
              air_val_ssa(c, fit_bool_result(c, in->base.type, nir_ine_imm(&c->b, u, 0))));
      return true;
   }

   if (!strcmp(op, "simd_broadcast_first")) {
      if (!a0)
         return air_error(c, "intrinsic: air.%s without an argument", n->part[0]);
      if (!is_cs) {
         air_set(c, &in->base, air_val_ssa(c, a0));
         return true;
      }
      air_note(c, "air.%s reads the first active lane of the SIMD group this "
                  "implementation has, which is not Metal's 32 lanes; the value is "
                  "the same one Metal would give wherever it is uniform across the "
                  "group, and another lane's where it is not", n->part[0]);
      air_set(c, &in->base,
              air_val_ssa(c, lane_move_vec(c, a0, NULL, AIR_MOVE_SIMD_FIRST)));
      return true;
   }

   if (!strcmp(op, "quad_broadcast_first")) {
      if (!a0)
         return air_error(c, "intrinsic: air.%s without an argument", n->part[0]);
      nir_def *res = is_vs ? a0 : lane_move_vec(c, a0, nir_imm_int(&c->b, 0),
                                                AIR_MOVE_QUAD_BRCST);
      air_set(c, &in->base, air_val_ssa(c, res));
      return true;
   }

   if (!strcmp(op, "simd_shuffle_and_fill_up") ||
       !strcmp(op, "simd_shuffle_and_fill_down") ||
       !strcmp(op, "quad_shuffle_and_fill_up") ||
       !strcmp(op, "quad_shuffle_and_fill_down")) {
      bool is_quad = !strncmp(op, "quad_", 5);
      bool is_up = strstr(op, "_up") != NULL;
      if (!a0 || in->num_operands <= 3)
         return air_error(c, "intrinsic: air.%s missing arguments", n->part[0]);
      nir_def *fill = air_get_ssa(c, in->operands[2]);
      nir_def *delta = air_get_ssa(c, in->operands[3]);
      if (!fill || !delta)
         return false;
      nir_def *d32 = nir_u2u32(&c->b, delta);
      nir_def *mod;
      if (in->num_operands > 4) {
         nir_def *m = air_get_ssa(c, in->operands[4]);
         if (!m)
            return false;
         mod = nir_u2u32(&c->b, m);
      } else if (is_quad) {
         mod = nir_imm_int(&c->b, 4);
      } else {
         mod = is_cs ? nir_load_subgroup_size(&c->b) : nir_imm_int(&c->b, 1);
      }

      if ((is_quad && is_vs) || (!is_quad && !is_cs)) {
         nir_def *keep = is_up ? nir_ieq_imm(&c->b, d32, 0) : nir_ult(&c->b, d32, mod);
         air_set(c, &in->base, air_val_ssa(c, nir_bcsel(&c->b, keep, a0, fill)));
         return true;
      }

      nir_def *lane = is_quad ? air_quad_lane_id(c) : nir_load_subgroup_invocation(&c->b);
      nir_def *mod_mask = nir_iadd_imm(&c->b, mod, -1);
      nir_def *idx = nir_iand(&c->b, lane, mod_mask);
      nir_def *base = nir_iand(&c->b, lane, nir_inot(&c->b, mod_mask));
      nir_def *use_fill = is_up ? nir_ult(&c->b, idx, d32)
                                : nir_uge(&c->b, nir_iadd(&c->b, idx, d32), mod);
      nir_def *ld = is_up ? nir_iadd(&c->b, base, nir_isub(&c->b, idx, d32))
                          : nir_iadd(&c->b, base, nir_iadd(&c->b, idx, d32));
      nir_def *lf = is_up ? nir_iadd(&c->b, base, nir_iadd(&c->b, nir_isub(&c->b, mod, d32), idx))
                          : nir_iadd(&c->b, base, nir_isub(&c->b, nir_iadd(&c->b, idx, d32), mod));
      nir_def *sd = is_quad ? quad_select_vec(c, a0, nir_iand_imm(&c->b, ld, 3))
                            : lane_move_vec(c, a0, ld, AIR_MOVE_SIMD_SHUFFLE);
      nir_def *sf = is_quad ? quad_select_vec(c, fill, nir_iand_imm(&c->b, lf, 3))
                            : lane_move_vec(c, fill, lf, AIR_MOVE_SIMD_SHUFFLE);
      air_set(c, &in->base, air_val_ssa(c, nir_bcsel(&c->b, use_fill, sf, sd)));
      return true;
   }

   if (!strcmp(op, "simd_broadcast") || !strcmp(op, "simd_shuffle") ||
       !strcmp(op, "simd_shuffle_xor") || !strcmp(op, "simd_shuffle_up") ||
       !strcmp(op, "simd_shuffle_down") || !strcmp(op, "simd_shuffle_rotate_up") ||
       !strcmp(op, "simd_shuffle_rotate_down")) {
      if (!a0 || in->num_operands <= 2)
         return air_error(c, "intrinsic: air.%s without data and lane arguments", n->part[0]);
      nir_def *arg1 = air_get_ssa(c, in->operands[2]);
      if (!arg1)
         return false;
      /* On Adreno 610 (!has_getfiberid), non-compute stages run with
       * subgroup_size = 1 and ir3 asserts if shuffle / subgroup_invocation
       * reaches codegen in fragment or vertex shaders.
       */
      if (!is_cs) {
         air_set(c, &in->base, air_val_ssa(c, a0));
         return true;
      }
      air_note(c, "air.%s takes its lane number in the SIMD group this "
                  "implementation has, which is not Metal's 32 lanes", n->part[0]);
      nir_def *u = nir_u2u32(&c->b, arg1);
      nir_def *res;
      if (!strcmp(op, "simd_shuffle_xor")) {
         res = lane_move_vec(c, a0, u, AIR_MOVE_SIMD_XOR);
      } else if (!strcmp(op, "simd_shuffle_up")) {
         res = lane_move_vec(c, a0, u, AIR_MOVE_SIMD_UP);
      } else if (!strcmp(op, "simd_shuffle_down")) {
         res = lane_move_vec(c, a0, u, AIR_MOVE_SIMD_DOWN);
      } else if (!strcmp(op, "simd_shuffle_rotate_up") ||
                 !strcmp(op, "simd_shuffle_rotate_down")) {
         bool rot_up = !strcmp(op, "simd_shuffle_rotate_up");
         nir_def *lane = nir_load_subgroup_invocation(&c->b);
         nir_def *wmask = nir_iadd_imm(&c->b, nir_load_subgroup_size(&c->b), -1);
         nir_def *target = rot_up ? nir_isub(&c->b, lane, u) : nir_iadd(&c->b, lane, u);
         res = lane_move_vec(c, a0, nir_iand(&c->b, target, wmask), AIR_MOVE_SIMD_SHUFFLE);
      } else {
         res = lane_move_vec(c, a0, u, AIR_MOVE_SIMD_SHUFFLE);
      }
      air_set(c, &in->base, air_val_ssa(c, res));
      return true;
   }

   if (!strcmp(op, "quad_broadcast") || !strcmp(op, "quad_shuffle") ||
       !strcmp(op, "quad_shuffle_xor") || !strcmp(op, "quad_shuffle_up") ||
       !strcmp(op, "quad_shuffle_down") || !strcmp(op, "quad_shuffle_rotate_up") ||
       !strcmp(op, "quad_shuffle_rotate_down")) {
      if (!a0 || in->num_operands <= 2)
         return air_error(c, "intrinsic: air.%s without data and lane arguments", n->part[0]);
      nir_def *arg1 = air_get_ssa(c, in->operands[2]);
      if (!arg1)
         return false;
      if (is_vs) {
         air_set(c, &in->base, air_val_ssa(c, a0));
         return true;
      }

      bool is_const = nir_src_is_const(nir_src_for_ssa(arg1));
      uint64_t cv = is_const ? nir_src_as_uint(nir_src_for_ssa(arg1)) : 0;
      nir_def *u = nir_u2u32(&c->b, arg1);
      nir_def *res = NULL;

      if (!strcmp(op, "quad_broadcast") || !strcmp(op, "quad_shuffle")) {
         if (is_const)
            res = lane_move_vec(c, a0, nir_imm_int(&c->b, (unsigned)(cv & 3)),
                                AIR_MOVE_QUAD_BRCST);
         else
            res = quad_select_vec(c, a0, nir_iand_imm(&c->b, u, 3));
      } else if (!strcmp(op, "quad_shuffle_xor")) {
         if (is_const) {
            switch (cv & 3) {
            case 0: res = a0; break;
            case 1: res = lane_move_vec(c, a0, NULL, AIR_MOVE_QUAD_HORIZ); break;
            case 2: res = lane_move_vec(c, a0, NULL, AIR_MOVE_QUAD_VERT); break;
            default: res = lane_move_vec(c, a0, NULL, AIR_MOVE_QUAD_DIAG); break;
            }
         } else {
            nir_def *tl = nir_iand_imm(&c->b, nir_ixor(&c->b, air_quad_lane_id(c), u), 3);
            res = quad_select_vec(c, a0, tl);
         }
      } else if (!strcmp(op, "quad_shuffle_up")) {
         if (is_const && (cv == 0 || cv >= 4)) {
            res = a0;
         } else if (is_const && cv == 2) {
            res = lane_move_vec(c, a0, NULL, AIR_MOVE_QUAD_VERT);
         } else if (is_const && cv == 3) {
            res = lane_move_vec(c, a0, nir_imm_int(&c->b, 0), AIR_MOVE_QUAD_BRCST);
         } else {
            nir_def *ql = air_quad_lane_id(c);
            nir_def *tl = nir_iand_imm(&c->b,
                             nir_imax(&c->b, nir_isub(&c->b, ql, u), nir_imm_int(&c->b, 0)),
                             3);
            res = quad_select_vec(c, a0, tl);
         }
      } else if (!strcmp(op, "quad_shuffle_down")) {
         if (is_const && (cv == 0 || cv >= 4)) {
            res = a0;
         } else if (is_const && cv == 2) {
            res = lane_move_vec(c, a0, NULL, AIR_MOVE_QUAD_VERT);
         } else if (is_const && cv == 3) {
            res = lane_move_vec(c, a0, nir_imm_int(&c->b, 3), AIR_MOVE_QUAD_BRCST);
         } else {
            nir_def *ql = air_quad_lane_id(c);
            nir_def *tl = nir_umin(&c->b, nir_iadd(&c->b, ql, u), nir_imm_int(&c->b, 3));
            res = quad_select_vec(c, a0, tl);
         }
      } else {
         /* quad_shuffle_rotate_up / quad_shuffle_rotate_down */
         bool rot_up = !strcmp(op, "quad_shuffle_rotate_up");
         if (is_const && (cv & 3) == 0) {
            res = a0;
         } else if (is_const && (cv & 3) == 2) {
            res = lane_move_vec(c, a0, NULL, AIR_MOVE_QUAD_VERT);
         } else {
            nir_def *ql = air_quad_lane_id(c);
            nir_def *tl = rot_up ? nir_isub(&c->b, ql, u) : nir_iadd(&c->b, ql, u);
            res = quad_select_vec(c, a0, nir_iand_imm(&c->b, tl, 3));
         }
      }

      air_set(c, &in->base, air_val_ssa(c, res));
      return true;
   }

   /* Reductions and inclusive/exclusive prefix scans:
    *   air.{simd,quad}_{sum,product,min,max,and,or,xor}
    *   air.{simd,quad}_prefix_{inclusive,exclusive}_{sum,product,min,max,and,or,xor}
    */
   if (!strncmp(op, "simd_", 5) || !strncmp(op, "quad_", 5)) {
      bool is_quad = !strncmp(op, "quad_", 5);
      bool is_f = air_type_is_float(air_type_scalar(in->base.type));
      bool is_signed = true;
      for (unsigned i = 1; i < n->num; i++) {
         if (!strcmp(n->part[i], "s")) {
            is_signed = true;
            break;
         }
         if (!strcmp(n->part[i], "u")) {
            is_signed = false;
            break;
         }
      }

      enum air_scan_kind skind;
      nir_op rop;
      if (parse_reduce_op(op + 5, is_f, is_signed, &skind, &rop)) {
         if (!a0)
            return air_error(c, "intrinsic: air.%s without an argument", n->part[0]);

         bool was_exact = c->b.exact;
         c->b.exact = false;
         nir_def *res = NULL;

         if ((is_quad && is_vs) || (!is_quad && !is_cs)) {
            res = (skind == AIR_SCAN_EXCLUSIVE)
                     ? reduce_identity_vec(c, rop, a0->num_components, a0->bit_size)
                     : a0;
         } else if (is_quad) {
            if (skind == AIR_SCAN_REDUCE) {
               nir_def *t = nir_build_alu2(&c->b, rop, a0,
                                           lane_move_vec(c, a0, NULL, AIR_MOVE_QUAD_HORIZ));
               res = nir_build_alu2(&c->b, rop, t,
                                    lane_move_vec(c, t, NULL, AIR_MOVE_QUAD_VERT));
            } else {
               nir_def *v0 = lane_move_vec(c, a0, nir_imm_int(&c->b, 0), AIR_MOVE_QUAD_BRCST);
               nir_def *v1 = lane_move_vec(c, a0, nir_imm_int(&c->b, 1), AIR_MOVE_QUAD_BRCST);
               nir_def *v2 = lane_move_vec(c, a0, nir_imm_int(&c->b, 2), AIR_MOVE_QUAD_BRCST);
               nir_def *v3 = lane_move_vec(c, a0, nir_imm_int(&c->b, 3), AIR_MOVE_QUAD_BRCST);
               nir_def *s1 = v0;
               nir_def *s2 = nir_build_alu2(&c->b, rop, s1, v1);
               nir_def *s3 = nir_build_alu2(&c->b, rop, s2, v2);
               nir_def *p0, *p1, *p2, *p3;
               if (skind == AIR_SCAN_EXCLUSIVE) {
                  p0 = reduce_identity_vec(c, rop, a0->num_components, a0->bit_size);
                  p1 = s1;
                  p2 = s2;
                  p3 = s3;
               } else {
                  p0 = s1;
                  p1 = s2;
                  p2 = s3;
                  p3 = nir_build_alu2(&c->b, rop, s3, v3);
               }
               nir_def *ql = air_quad_lane_id(c);
               nir_def *b0 = nir_ine_imm(&c->b, nir_iand_imm(&c->b, ql, 1), 0);
               nir_def *b1 = nir_ine_imm(&c->b, nir_iand_imm(&c->b, ql, 2), 0);
               res = nir_bcsel(&c->b, b1,
                               nir_bcsel(&c->b, b0, p3, p2),
                               nir_bcsel(&c->b, b0, p1, p0));
            }
         } else {
            air_note(c, "air.%s is computed over the SIMD group this implementation has",
                     n->part[0]);
            if (a0->num_components == 1) {
               res = emit_simd_scan_scalar(c, a0, rop, is_signed, skind, n->part[0]);
            } else {
               nir_def *comps[NIR_MAX_VEC_COMPONENTS];
               for (unsigned i = 0; i < a0->num_components; i++)
                  comps[i] = emit_simd_scan_scalar(c, nir_channel(&c->b, a0, i),
                                                   rop, is_signed, skind, n->part[0]);
               res = nir_vec(&c->b, comps, a0->num_components);
            }
         }

         c->b.exact = was_exact;
         air_set(c, &in->base, air_val_ssa(c, res));
         return true;
      }
   }

   *handled = false;
   return true;
}

/* -- llvm.* ------------------------------------------------------------- */

static bool
llvm_op(struct air_ctx *c, const struct air_inst *in, const char *name)
{
   if (!strncmp(name, "llvm.lifetime.", 14))
      return true; /* scope markers: nothing to emit */
   if (!strncmp(name, "llvm.memcpy.", 12)) {
      if (in->num_operands < 4)
         return air_error(c, "intrinsic: memcpy of %u arguments", in->num_operands - 1);
      /* a copy between two of our pointers, of a constant length */
      struct air_val *dst = air_get(c, in->operands[1]);
      struct air_val *src = air_get(c, in->operands[2]);
      if (!dst || !src)
         return false;
      if (dst->kind != AIR_VAL_PTR || src->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: memcpy that is not between pointers");
      uint64_t bytes;
      if (!air_const_uint(in->operands[3], &bytes))
         return air_error(c, "intrinsic: memcpy of a run-time length");
      /* in 4-byte pieces, which is what every memcpy in the corpus is a
       * multiple of (they copy float vectors out of constant buffers)
       */
      if (bytes % 4)
         return air_error(c, "intrinsic: memcpy of %u bytes", (unsigned)bytes);
      for (uint64_t off = 0; off < bytes; off += 4) {
         struct air_ptr s = air_ptr_add(c, src->ptr, nir_imm_int(&c->b, off));
         struct air_ptr d = air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off));
         static const struct air_type u32 = { .kind = AIR_TYPE_INT, .bits = 32 };
         nir_def *v = air_load_ptr(c, s, &u32, 4);
         if (!v || !air_store_ptr(c, d, v, 4))
            return false;
      }
      return true;
   }
   if (!strncmp(name, "llvm.memset.", 12)) {
      if (in->num_operands < 4)
         return air_error(c, "intrinsic: memset of %u arguments", in->num_operands - 1);
      struct air_val *dst = air_get(c, in->operands[1]);
      nir_def *byte = air_get_ssa(c, in->operands[2]);
      if (!dst || !byte)
         return false;
      if (dst->kind != AIR_VAL_PTR)
         return air_error(c, "intrinsic: memset of something that is not a pointer");
      uint64_t bytes;
      if (!air_const_uint(in->operands[3], &bytes) || bytes % 4)
         return air_error(c, "intrinsic: memset of a length that is not a multiple of 4");
      if (!nir_src_is_const(nir_src_for_ssa(byte)))
         return air_error(c, "intrinsic: memset of a run-time byte");
      uint32_t b = nir_src_as_uint(nir_src_for_ssa(byte)) & 0xff;
      uint32_t word = b | b << 8 | b << 16 | b << 24;
      for (uint64_t off = 0; off < bytes; off += 4) {
         struct air_ptr d = air_ptr_add(c, dst->ptr, nir_imm_int(&c->b, off));
         if (!air_store_ptr(c, d, nir_imm_int(&c->b, word), 4))
            return false;
      }
      return true;
   }
   if (!strncmp(name, "llvm.fmuladd.", 13)) {
      if (in->num_operands < 4)
         return air_error(c, "intrinsic: fmuladd of %u arguments", in->num_operands - 1);
      nir_def *a = air_get_ssa(c, in->operands[1]);
      nir_def *b = air_get_ssa(c, in->operands[2]);
      nir_def *d = air_get_ssa(c, in->operands[3]);
      if (!a || !b || !d)
         return false;
      air_set(c, &in->base, air_val_ssa(c, nir_ffma(&c->b, a, b, d)));
      return true;
   }
   if (!strncmp(name, "llvm.fabs.", 10)) {
      if (in->num_operands < 2)
         return air_error(c, "intrinsic: fabs of %u arguments", in->num_operands - 1);
      nir_def *a = air_get_ssa(c, in->operands[1]);
      if (!a)
         return false;
      air_set(c, &in->base, air_val_ssa(c, nir_fabs(&c->b, a)));
      return true;
   }
   if (!strcmp(name, "llvm.trap"))
      return air_error(c, "intrinsic: llvm.trap");
   return air_error(c, "intrinsic: %s", name);
}

/* -- textures ------------------------------------------------------------
 *
 * The largest family and the one with the long tail. Its vocabulary is
 * Apple's, read off AGXCompilerCore the same way the arithmetic was
 * (docs/air.md section 3.2): the names there end in an underscore --
 * air.sample_texture_, air.gather_compare_depth_, air.get_num_mip_levels_texture_
 * -- because the dimensionality is appended to them. That is why the earlier
 * survey counted "about 117 get_*_texture_* variants": it is some twenty
 * operations crossed with nine dimensionalities, not a hundred separate
 * things.
 *
 * The signatures are measured from the corpus, not guessed:
 *
 *   sample_<kind>_<dim> (tex, samp, [plane,] coord, [i32 array,]
 *                        [i1 has_offset, offset,]
 *                        i1 lod_is_level, float lod_or_bias, float min_lod,
 *                        i32 flags)                  -> { <4 x T>, i8 }
 *   sample_<kind>_<dim>_grad
 *                       (tex, samp, [plane,] coord, [i32 array,]
 *                        dPdx, dPdy, float min_lod,
 *                        [i1 has_offset, offset,] i32 flags)
 *   gather_<kind>_<dim> (tex, samp, coord, [i32 array,]
 *                        [i1 has_offset, offset,]
 *                        i32 component, i32 flags)   -> { <4 x T>, i8 }
 *   read_<kind>_<dim>   (tex, [plane,] coord, [i32 array,]
 *                        i32 lod_or_sample, i32 flags)
 *   write_<kind>_<dim>  (tex, coord, [i32 array,] value, i32 lod, i32 flags)
 *   get_<what>_<kind>_<dim> (tex[, i32 lod])         -> i32
 *
 * Three things in those brackets, and none of them is a guess. They are read
 * off Apple's own metal_stdlib on this device --
 * GPUCompiler.framework/.../include/metal/metal_texture, where every
 * `texture2d_array<T>::sample` and its fifty neighbours are one-line
 * forwarders to the builtin, so the header states the argument ORDER for all
 * nine dimensionalities and six operations -- and then confirmed against the
 * LLVM declaration in the modules that call them. The two agree everywhere:
 *
 *   AN ARRAY INDEX IS ITS OWN ARGUMENT, straight after the coordinate, in
 *   every one of sample, gather, read and write. It is not inside the
 *   coordinate vector, which is what this file used to require of it:
 *   air.sample_texture_2d_array takes a <2 x float> and an i32, and the
 *   refusal it used to earn said "with a 2-component coordinate, wanting 3".
 *
 *   THE GRADIENT FORM REORDERS WHAT FOLLOWS. dPdx, dPdy and the minimum level
 *   come before the offset pair, where the plain form has the offset pair
 *   first and the level last. A reader that assumed one order for both would
 *   take a gradient for an offset flag.
 *
 *   A CUBE HAS NO OFFSET PAIR AT ALL -- Metal defines no offset for cube
 *   sampling -- so air.sample_texture_cube goes from the coordinate straight
 *   to the level flag. Reading the 2d shape there is what made 621 modules
 *   stop at "argument 5 is not a constant": the float minimum level was being
 *   read as the i1 that says whether the next float is a level.
 *
 * The i1 before the level is read as "the float is a level, not a bias", so
 * (false, 0.0) is a plain sample -- which is what every one of the system
 * corpus's sample calls passes. Resident Evil 4 exercises both: 23 823
 * level-form calls against 5 961 bias-form.
 *
 * The second field of the returned struct is Metal's sparse-residency byte.
 * No module in the corpus reads it (every extractvalue of a texture result
 * takes field 0), so it is produced as zero rather than invented.
 */

struct air_tex_name {
   const char *op;        /* "sample", "gather", "read", "write", "get", "lod" */
   const char *query;     /* for "get": width, height, depth, array_size, ... */
   bool compare;          /* sample_compare / gather_compare */
   bool depth;            /* a depth texture rather than a colour one */
   enum glsl_sampler_dim dim;
   bool is_array, is_ms, is_buffer;
   bool grad;             /* the _grad / gradient_ form: explicit dPdx and dPdy */
   bool explicit_lod;     /* _lod / lod_ explicit mip-level form */
   bool no_offset;        /* this dimensionality takes no offset argument */
   bool unclamped_lod;    /* calculate_unclamped_lod_* */
};

static bool
eat(const char **s, const char *prefix)
{
   size_t n = strlen(prefix);
   if (strncmp(*s, prefix, n))
      return false;
   *s += n;
   return true;
}

/* air.<op>_<kind>_<dim> -> the pieces, or false when it is not a texture name */
static bool
parse_tex_name(const char *name, struct air_tex_name *t)
{
   memset(t, 0, sizeof(*t));
   const char *s = name;

   if (eat(&s, "sample_compare_")) {
      t->op = "sample";
      t->compare = true;
   } else if (eat(&s, "sample_")) {
      t->op = "sample";
   } else if (eat(&s, "gather_compare_")) {
      t->op = "gather";
      t->compare = true;
   } else if (eat(&s, "gather_")) {
      t->op = "gather";
   } else if (eat(&s, "read_")) {
      t->op = "read";
   } else if (eat(&s, "write_")) {
      t->op = "write";
   } else if (eat(&s, "calculate_clamped_lod_")) {
      t->op = "lod";
   } else if (eat(&s, "calculate_unclamped_lod_")) {
      t->op = "lod";
      t->unclamped_lod = true;
   } else if (eat(&s, "get_")) {
      t->op = "get";
      static const char *queries[] = {
         "num_mip_levels_", "num_samples_", "array_size_", "width_", "height_", "depth_",
      };
      static const char *names[] = {
         "num_mip_levels", "num_samples", "array_size", "width", "height", "depth",
      };
      for (unsigned i = 0; i < ARRAY_SIZE(queries); i++) {
         if (eat(&s, queries[i])) {
            t->query = names[i];
            break;
         }
      }
      if (!t->query)
         return false;
   } else {
      return false;
   }

   if (!strcmp(t->op, "sample")) {
      if (eat(&s, "gradient_"))
         t->grad = true;
      else if (eat(&s, "lod_"))
         t->explicit_lod = true;
   }

   if (eat(&s, "texture_buffer_1d")) {
      t->dim = GLSL_SAMPLER_DIM_BUF;
      t->is_buffer = true;
      t->no_offset = true;
      return *s == 0;
   }
   if (eat(&s, "depth_"))
      t->depth = true;
   else if (eat(&s, "texture_"))
      t->depth = false;
   else if (t->compare)
      t->depth = true;
   else
      return false;

   if (!strcmp(t->op, "sample")) {
      if (eat(&s, "gradient_"))
         t->grad = true;
      else if (eat(&s, "lod_"))
         t->explicit_lod = true;
   }

   /* The gradient form appends _grad to the DIMENSIONALITY and not to the
    * operation: Apple's builtin is __metal_sample_texture_2d_array_t_grad, so
    * the air.* name is air.sample_texture_2d_array_grad. Taking it off here
    * rather than giving the table nine more rows is what keeps one name for
    * one dimensionality -- and the exact comparison below is why 858 modules
    * used to stop at "a texture name this cannot take apart".
    */
   size_t len = strlen(s);
   if (len > 5 && !strcmp(s + len - 5, "_grad")) {
      t->grad = true;
      len -= 5;
   } else if (len > 9 && !strcmp(s + len - 9, "_gradient")) {
      t->grad = true;
      len -= 9;
   } else if (len > 4 && !strcmp(s + len - 4, "_lod")) {
      t->explicit_lod = true;
      len -= 4;
   }

   /* the dimensionality, and whether it has an offset argument at all */
   static const struct {
      const char *suffix;
      enum glsl_sampler_dim dim;
      bool array, ms, no_offset;
   } dims[] = {
      { "cube_array", GLSL_SAMPLER_DIM_CUBE, true, false, true },
      { "2d_ms_array", GLSL_SAMPLER_DIM_MS, true, true, false },
      { "2d_array", GLSL_SAMPLER_DIM_2D, true, false, false },
      { "1d_array", GLSL_SAMPLER_DIM_1D, true, false, false },
      { "2d_ms", GLSL_SAMPLER_DIM_MS, false, true, false },
      { "cube", GLSL_SAMPLER_DIM_CUBE, false, false, true },
      { "1d", GLSL_SAMPLER_DIM_1D, false, false, false },
      { "2d", GLSL_SAMPLER_DIM_2D, false, false, false },
      { "3d", GLSL_SAMPLER_DIM_3D, false, false, false },
   };
   for (unsigned i = 0; i < ARRAY_SIZE(dims); i++) {
      if (strlen(dims[i].suffix) == len && !strncmp(s, dims[i].suffix, len)) {
         t->dim = dims[i].dim;
         t->is_array = dims[i].array;
         t->is_ms = dims[i].ms;
         t->no_offset = dims[i].no_offset;
         return true;
      }
   }
   return false;
}

static unsigned
coord_components_of(const struct air_tex_name *t)
{
   unsigned n = t->dim == GLSL_SAMPLER_DIM_1D ? 1 :
                t->dim == GLSL_SAMPLER_DIM_3D ? 3 :
                t->dim == GLSL_SAMPLER_DIM_CUBE ? 3 :
                t->dim == GLSL_SAMPLER_DIM_BUF ? 1 : 2;
   return n + (t->is_array ? 1 : 0);
}

/* the result type of a texture operation, from the LLVM type it returns */
static nir_alu_type
tex_dest_type(struct air_ctx *c, const struct air_type *ret, const struct air_name *n)
{
   const struct air_type *t = ret;
   if (t->kind == AIR_TYPE_STRUCT && t->num_members)
      t = t->members[0];
   const struct air_type *scalar = air_type_scalar(t);
   unsigned bits = air_type_bit_size(scalar);
   if (air_type_is_float(scalar))
      return nir_type_float | bits;
   /* the signedness is in the name, not in the type: .s or .u */
   for (unsigned i = 1; i < n->num; i++) {
      if (!strcmp(n->part[i], "s"))
         return nir_type_int | bits;
      if (!strcmp(n->part[i], "u"))
         return nir_type_uint | bits;
   }
   return nir_type_uint | bits;
}

/* The pointer a texture or sampler argument carries. */
static bool
tex_handle(struct air_ctx *c, const struct air_inst *in, unsigned operand,
           enum air_ptr_class want, struct air_ptr *out)
{
   if (operand >= in->num_operands)
      return air_error(c, "intrinsic: missing %s argument %u",
                       want == AIR_PTR_TEXTURE ? "texture" : "sampler", operand - 1);
   struct air_val *v = air_get(c, in->operands[operand]);
   if (!v)
      return false;
   if (v->kind != AIR_VAL_PTR || v->ptr.class != want)
      return air_error(c, "intrinsic: argument %u is not a %s", operand - 1,
                       want == AIR_PTR_TEXTURE ? "texture" : "sampler");
   *out = v->ptr;
   return true;
}

/* a constant i1/i32 argument, which these intrinsics use as flags */
static bool
tex_flag(struct air_ctx *c, const struct air_inst *in, unsigned operand, uint64_t *out)
{
   if (operand >= in->num_operands)
      return air_error(c, "intrinsic: missing constant argument %u", operand - 1);
   if (!air_const_uint(in->operands[operand], out))
      return air_error(c, "intrinsic: argument %u is not a constant", operand - 1);
   return true;
}

/* the { value, residency } struct these return */
static void
set_tex_result(struct air_ctx *c, const struct air_inst *in, nir_def *value)
{
   const struct air_type *ret = in->base.type;
   if (ret->kind != AIR_TYPE_STRUCT) {
      air_set(c, &in->base, air_val_ssa(c, value));
      return;
    }
   struct air_val *agg = rzalloc(c->mem_ctx, struct air_val);
   agg->kind = AIR_VAL_AGG;
   agg->num_elems = ret->num_members;
   agg->elems = rzalloc_array(c->mem_ctx, struct air_val *, MAX2(ret->num_members, 1));
   agg->elems[0] = air_val_ssa(c, value);
   for (unsigned i = 1; i < ret->num_members; i++) {
      /* the sparse-residency byte: nothing in the corpus reads it */
      unsigned bits = air_nir_bit_size(c, ret->members[i]);
      agg->elems[i] = air_val_ssa(c, nir_imm_intN_t(&c->b, 0, bits));
   }
   air_set(c, &in->base, agg);
}


/* A query: a size, a level count or a sample count, of a texture or an image. */
static nir_def *
query_texture(struct air_ctx *c, const struct air_ptr *tex, const struct air_tex_name *t,
              nir_texop op, nir_def *lod, unsigned comps)
{
   if (tex->is_image) {
      nir_def *index = nir_imm_int(&c->b, tex->binding);
      switch (op) {
      case nir_texop_txs: {
         /* an image size has as many components as the image has dimensions;
          * the caller passes 0 for "as many as there are"
          */
         unsigned n = comps ? comps :
            (t->dim == GLSL_SAMPLER_DIM_1D || t->dim == GLSL_SAMPLER_DIM_BUF ? 1 :
             t->dim == GLSL_SAMPLER_DIM_3D ? 3 : 2) + (t->is_array ? 1 : 0);
         return nir_image_size(&c->b, n, 32, index, lod ? lod : nir_imm_int(&c->b, 0),
                               .image_dim = t->dim, .image_array = t->is_array);
      }
      case nir_texop_query_levels:
         /* An image binding views a single mip level, and ir3 does not lower
          * nir_intrinsic_image_levels.
          */
         return nir_imm_int(&c->b, 1);
      default:
         if (!t->is_ms)
            return nir_imm_int(&c->b, 1);
         return nir_image_samples(&c->b, 32, index, .image_dim = t->dim,
                                  .image_array = t->is_array);
      }
   }

   if (op == nir_texop_query_levels && (t->is_buffer || t->is_ms))
      return nir_imm_int(&c->b, 1);
   if (op == nir_texop_texture_samples && !t->is_ms)
      return nir_imm_int(&c->b, 1);

   nir_tex_instr *q = nir_tex_instr_create(c->nir, lod ? 1 : 0);
   q->op = op;
   q->sampler_dim = t->dim;
   q->is_array = t->is_array;
   q->dest_type = nir_type_int32;
   q->texture_index = tex->binding;
   q->sampler_index = 0;
   if (lod)
      q->src[0] = (nir_tex_src){ .src_type = nir_tex_src_lod, .src = nir_src_for_ssa(lod) };
   /* NIR fixes the size of a query's result itself -- txs is two components
    * for a 2D texture whatever the caller asks one of them for -- so it is
    * asked here and the component picked afterwards.
    */
   if (comps == 0)
      comps = nir_tex_instr_dest_size(q);
   nir_def_init(&q->instr, &q->def, comps, 32);
   nir_builder_instr_insert(&c->b, &q->instr);
   return &q->def;
}

/* is every component of this value a constant zero? */
static bool
all_zero(nir_def *def)
{
   if (!nir_src_is_const(nir_src_for_ssa(def)))
      return false;
   for (unsigned i = 0; i < def->num_components; i++)
      if (nir_src_comp_as_int(nir_src_for_ssa(def), i) != 0)
         return false;
   return true;
}

/* The coordinate, with the array index that follows it folded into it.
 *
 * NIR wants one coordinate with the layer as its last component; AIR passes
 * the two separately (see the signatures above). Which way the layer is
 * converted follows the coordinate's own type, which is also the only place
 * the answer can come from: a sample's coordinate is floating point and NIR's
 * array layer is then a float like the rest of it, while a read's is an
 * integer and the layer stays one.
 *
 * The float conversion is exact and not a rounding to be worried about. Metal
 * hands over an integer layer; u2f of an integer below 2^24 is exact in a
 * float, and 2^24 layers is 16 million slices of a texture array whose limit
 * on this hardware is 2 048. What a backend does with a *fractional* layer --
 * Vulkan says round to even, GLSL says nearest -- therefore never arises
 * here, because the value handed to it is always a whole number.
 *
 * Both integer and half-float coordinates are widened to 32 bits first so that
 * array layers, comparators, gradients, offsets and LOD/bias operands never
 * mix half and full registers inside ir3's collect instructions; ir3's own
 * nir_opt_16bit_tex_image pass narrows texture sources back to 16-bit as a
 * matched group when all sources on the instruction allow it.
 */
static nir_def *
tex_coord_and_layer(struct air_ctx *c, const struct air_inst *in,
                    const struct air_tex_name *t, const struct air_name *n,
                    unsigned *arg, unsigned ncoord)
{
   if (in->num_operands <= *arg) {
      air_error(c, "intrinsic: air.%s without a coordinate", n->part[0]);
      return NULL;
   }
   const struct air_value *cv = in->operands[*arg];
   nir_def *coord = air_get_ssa(c, cv);
   if (!coord)
      return NULL;
   (*arg)++;
   bool is_float = air_type_is_float(air_type_scalar(cv->type));
   if (!is_float)
      coord = nir_u2u32(&c->b, coord);
   else if (coord->bit_size != 32)
      coord = nir_f2f32(&c->b, coord);

   if (t->is_array) {
      if (in->num_operands <= *arg) {
         air_error(c, "intrinsic: air.%s without the array index its signature has",
                   n->part[0]);
         return NULL;
      }
      nir_def *layer = air_get_ssa(c, in->operands[(*arg)++]);
      if (!layer)
         return NULL;
      if (layer->num_components != 1) {
         air_error(c, "intrinsic: air.%s with a %u-component array index",
                   n->part[0], layer->num_components);
         return NULL;
      }
      layer = is_float ? nir_u2fN(&c->b, layer, coord->bit_size)
                       : nir_u2u32(&c->b, layer);

      nir_def *comps[4];
      unsigned nc = MIN2(coord->num_components, 3);
      for (unsigned i = 0; i < nc; i++)
         comps[i] = nir_channel(&c->b, coord, i);
      comps[nc] = layer;
      coord = nir_vec(&c->b, comps, nc + 1);
   }

   if (coord->num_components != ncoord) {
      air_error(c, "intrinsic: air.%s with a %u-component coordinate, wanting %u",
                n->part[0], coord->num_components, ncoord);
      return NULL;
   }
   return coord;
}

static nir_def *
emit_single_tex(struct air_ctx *c, nir_texop op, const struct air_tex_name *t,
                nir_alu_type dest_type, unsigned dest_bits, unsigned want_coord,
                unsigned tex_binding, unsigned samp_binding, unsigned component,
                const nir_tex_src *srcs, unsigned nsrc)
{
   nir_tex_instr *tex_instr = nir_tex_instr_create(c->nir, nsrc);
   tex_instr->op = op;
   tex_instr->sampler_dim = t->dim;
   tex_instr->is_array = op == nir_texop_lod ? false : t->is_array;
   tex_instr->is_shadow = t->compare;
   tex_instr->is_new_style_shadow = t->compare && op != nir_texop_tg4;
   tex_instr->dest_type = dest_type;
   tex_instr->coord_components = want_coord;
   tex_instr->texture_index = tex_binding;
   tex_instr->sampler_index = samp_binding;
   tex_instr->component = component;
   for (unsigned i = 0; i < nsrc; i++)
      tex_instr->src[i] = srcs[i];

   unsigned nir_comps = nir_tex_instr_dest_size(tex_instr);
   nir_def_init(&tex_instr->instr, &tex_instr->def, nir_comps, dest_bits);
   nir_builder_instr_insert(&c->b, &tex_instr->instr);
   return &tex_instr->def;
}

static bool
texture_op(struct air_ctx *c, const struct air_inst *in, const struct air_name *n)
{
   struct air_tex_name t;
   if (!parse_tex_name(n->part[0], &t))
      return air_error(c, "intrinsic: air.%s (a texture name this cannot take apart)",
                       n->part[0]);
   if (t.is_buffer && strcmp(t.op, "read") && strcmp(t.op, "write") &&
       strcmp(t.op, "get"))
      return air_error(c, "intrinsic: air.%s (unsupported operation %s on texture buffer)",
                       n->part[0], t.op);
   if (t.compare && !t.depth)
      return air_error(c, "intrinsic: air.%s (a comparison of a colour texture, which "
                          "Metal declares on depth textures only)", n->part[0]);
   if (t.compare && strcmp(t.op, "sample") && strcmp(t.op, "gather"))
      return air_error(c, "intrinsic: air.%s (a comparison form of %s)",
                       n->part[0], t.op);
   /* Only a sample has a gradient form -- Apple's header has
    * __metal_sample_*_t_grad and no gather, read or write counterpart -- and a
    * name that claimed one would be read by the sample branch's rules below,
    * which is the kind of silent misparse this file refuses instead.
    */
   if (t.grad && strcmp(t.op, "sample"))
      return air_error(c, "intrinsic: air.%s (a gradient form of %s, which Metal has no "
                          "signature for)", n->part[0], t.op);

   struct air_ptr tex;
   if (!tex_handle(c, in, 1, AIR_PTR_TEXTURE, &tex))
      return false;

   unsigned arg = 2;   /* operands[0] is the callee, [1] the texture */
   struct air_ptr samp = { 0 };
   bool have_sampler = !strcmp(t.op, "sample") || !strcmp(t.op, "gather") ||
                       !strcmp(t.op, "lod");
   if (have_sampler) {
      if (!tex_handle(c, in, arg, AIR_PTR_SAMPLER, &samp))
         return false;
      arg++;
   }

   /* A depth sample, gather or read takes a scalar i32 plane/aspect selector
    * before the coordinate, and every call passes 1. Size/level queries on
    * depth textures do not carry a plane argument.
    */
   if (t.depth && strcmp(t.op, "get") != 0 && strcmp(t.op, "write") != 0 &&
       arg < in->num_operands && in->operands[arg]->type->kind == AIR_TYPE_INT &&
       (strcmp(t.op, "read") != 0 || t.dim != GLSL_SAMPLER_DIM_1D)) {
      uint64_t plane;
      if (!tex_flag(c, in, arg, &plane))
         return false;
      if (plane != 1)
         return air_error(c, "intrinsic: air.%s with aspect %u (only 1 is understood)",
                          n->part[0], (unsigned)plane);
      arg++;
   }

   unsigned ncoord = coord_components_of(&t);
   nir_alu_type dest_type = tex_dest_type(c, in->base.type, n);
   unsigned dest_bits = nir_alu_type_get_type_size(dest_type);

   if (!strcmp(t.op, "get")) {
      /* the queries: a size, a level count or a sample count */
      nir_def *res;
      if (!strcmp(t.query, "num_samples")) {
         res = query_texture(c, &tex, &t, nir_texop_texture_samples, NULL, 1);
      } else if (!strcmp(t.query, "num_mip_levels")) {
         res = query_texture(c, &tex, &t, nir_texop_query_levels, NULL, 1);
      } else {
         nir_def *lod = nir_imm_int(&c->b, 0);
         if (!t.is_buffer && !t.is_ms && in->num_operands > arg) {
            nir_def *given = air_get_ssa(c, in->operands[arg]);
            if (!given)
               return false;
            lod = nir_u2u32(&c->b, given);
         }
         nir_def *size = query_texture(c, &tex, &t, nir_texop_txs, lod, 0);
         unsigned comp = !strcmp(t.query, "width") ? 0 :
                         !strcmp(t.query, "height") ? 1 :
                         !strcmp(t.query, "depth") ? 2 : size->num_components - 1;
         if (!strcmp(t.query, "array_size") && !t.is_array)
            return air_error(c, "intrinsic: air.%s on a texture that is not an array",
                             n->part[0]);
         if (comp >= size->num_components)
            return air_error(c, "intrinsic: air.%s asks for component %u of %u",
                             n->part[0], comp, size->num_components);
         res = nir_channel(&c->b, size, comp);
      }
      air_set(c, &in->base, air_val_ssa(c, nir_u2uN(&c->b, res, dest_bits)));
      return true;
   }

   if (!strcmp(t.op, "write")) {
      nir_def *coord = tex_coord_and_layer(c, in, &t, n, &arg, ncoord);
      if (!coord)
         return false;
      if (in->num_operands <= arg)
         return air_error(c, "intrinsic: air.%s without a value", n->part[0]);
      const struct air_value *val_op = in->operands[arg++];
      nir_def *value = air_get_ssa(c, val_op);
      if (!value)
         return false;
      /* Mipmapped textures pass (lod, access_flags) after value; non-mipmapped
       * texture_buffer_1d and multisampled textures pass only access_flags.
       */
      if (!t.is_buffer && !t.is_ms && in->num_operands >= arg + 2) {
         uint64_t lod;
         if (!tex_flag(c, in, arg, &lod))
            return false;
         if (lod != 0)
            return air_error(c, "intrinsic: air.%s to mip level %u (only 0 is handled)",
                             n->part[0], (unsigned)lod);
      }
      if (!tex.is_image)
         return air_error(c, "intrinsic: air.%s to a texture the interface says is not "
                             "written", n->part[0]);
      nir_alu_type store_type = tex_dest_type(c, val_op->type, n);
      air_mark_written(c, AIR_RESOURCE_IMAGE, tex.binding);
      nir_image_store(&c->b, nir_imm_int(&c->b, tex.binding),
                      nir_pad_vector(&c->b, nir_u2u32(&c->b, coord), 4),
                      nir_imm_int(&c->b, 0), nir_pad_vector(&c->b, value, 4),
                      nir_imm_int(&c->b, 0),
                      .image_dim = t.dim, .image_array = t.is_array,
                      .src_type = store_type);
      return true;
   }

   /* sample, gather, lod, read: a texture instruction, or an image load */
   struct air_tex_name t_coord = t;
   unsigned want_coord = ncoord;
   if (!strcmp(t.op, "lod")) {
      t_coord.is_array = false;
      want_coord = ncoord - (t.is_array ? 1 : 0);
   }
   nir_def *coord = tex_coord_and_layer(c, in, &t_coord, n, &arg, want_coord);
   if (!coord)
      return false;

   if (!strcmp(t.op, "read") && tex.is_image) {
      nir_def *zero = nir_imm_int(&c->b, 0);
      nir_def *which = zero;
      if (!t.is_buffer && in->num_operands >= arg + 2) {
         which = air_get_ssa(c, in->operands[arg]);
         if (!which)
            return false;
         which = nir_u2u32(&c->b, which);
      }
      nir_def *res = nir_image_load(&c->b, 4, dest_bits, nir_imm_int(&c->b, tex.binding),
                                    nir_pad_vector(&c->b, nir_u2u32(&c->b, coord), 4),
                                    t.is_ms ? which : zero,
                                    t.is_ms ? zero : which,
                                    .image_dim = t.dim, .image_array = t.is_array,
                                    .dest_type = dest_type);
      set_tex_result(c, in, res);
      return true;
   }

   nir_def *compare = NULL;
   if (t.compare) {
      if (in->num_operands <= arg)
         return air_error(c, "intrinsic: air.%s without the value its signature compares "
                             "against", n->part[0]);
      if (!(compare = air_get_ssa(c, in->operands[arg++])))
         return false;
      if (compare->num_components != 1)
         return air_error(c, "intrinsic: air.%s compares against %u components, and a "
                             "depth comparison takes one", n->part[0],
                          compare->num_components);
      if (compare->bit_size != coord->bit_size)
         compare = nir_f2fN(&c->b, compare, coord->bit_size);
   }

   unsigned grad_comps = ncoord - (t.is_array ? 1 : 0);
   nir_tex_src srcs[7];
   unsigned nsrc = 0;
   srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_coord,
                                 .src = nir_src_for_ssa(coord) };
   if (compare)
      srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_comparator,
                                    .src = nir_src_for_ssa(compare) };

   nir_texop op;
   if (!strcmp(t.op, "sample")) {
      uint64_t has_offset = 0, is_level = t.explicit_lod ? 1 : 0;
      nir_def *offset = NULL, *ddx = NULL, *ddy = NULL, *min_lod = NULL, *level = NULL;

      if (t.grad) {
         if (in->num_operands < arg + 2)
            return air_error(c, "intrinsic: air.%s missing gradient arguments", n->part[0]);
         ddx = air_get_ssa(c, in->operands[arg++]);
         ddy = air_get_ssa(c, in->operands[arg++]);
         if (!ddx || !ddy)
            return false;
         if (ddx->num_components != grad_comps || ddy->num_components != grad_comps)
            return air_error(c, "intrinsic: air.%s with (%u, %u)-component gradients, "
                                "wanting %u", n->part[0], ddx->num_components,
                             ddy->num_components, grad_comps);
         if (ddx->bit_size != coord->bit_size)
            ddx = nir_f2fN(&c->b, ddx, coord->bit_size);
         if (ddy->bit_size != coord->bit_size)
            ddy = nir_f2fN(&c->b, ddy, coord->bit_size);

         if (arg < in->num_operands &&
             air_type_is_float(air_type_scalar(in->operands[arg]->type))) {
            min_lod = air_get_ssa(c, in->operands[arg++]);
            if (!min_lod)
               return false;
         } else {
            min_lod = nir_imm_floatN_t(&c->b, 0.0, coord->bit_size);
         }

         if (!t.no_offset && arg < in->num_operands &&
             in->operands[arg]->type->kind == AIR_TYPE_INT &&
             in->operands[arg]->type->bits == 1) {
            if (!tex_flag(c, in, arg, &has_offset))
               return false;
            arg++;
            if (in->num_operands <= arg || !(offset = air_get_ssa(c, in->operands[arg++])))
               return in->num_operands <= arg
                         ? air_error(c, "intrinsic: air.%s missing offset", n->part[0])
                         : false;
         }
      } else if (t.explicit_lod && arg < in->num_operands &&
                 air_type_is_float(air_type_scalar(in->operands[arg]->type))) {
         level = air_get_ssa(c, in->operands[arg++]);
         if (!level)
            return false;
         is_level = 1;
         min_lod = nir_imm_floatN_t(&c->b, 0.0, coord->bit_size);
      } else {
         if (!t.no_offset) {
            if (!tex_flag(c, in, arg, &has_offset))
               return false;
            arg++;
            if (in->num_operands <= arg || !(offset = air_get_ssa(c, in->operands[arg++])))
               return in->num_operands <= arg
                         ? air_error(c, "intrinsic: air.%s missing offset", n->part[0])
                         : false;
         }
         if (!tex_flag(c, in, arg, &is_level))
            return false;
         arg++;
         if (t.explicit_lod)
            is_level = 1;
         if (in->num_operands < arg + 2)
            return air_error(c, "intrinsic: air.%s missing lod arguments", n->part[0]);
         level = air_get_ssa(c, in->operands[arg++]);
         min_lod = air_get_ssa(c, in->operands[arg++]);
         if (!level || !min_lod)
            return false;
      }

      if (offset && has_offset && !all_zero(offset)) {
         if (offset->num_components != grad_comps)
            return air_error(c, "intrinsic: air.%s with a %u-component offset, wanting %u",
                             n->part[0], offset->num_components, grad_comps);
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_offset,
                                       .src = nir_src_for_ssa(nir_i2i32(&c->b, offset)) };
      }

      bool min_lod_is_zero = nir_src_is_const(nir_src_for_ssa(min_lod)) &&
                             nir_src_as_float(nir_src_for_ssa(min_lod)) == 0.0;

      if (t.grad) {
         if (t.compare && t.dim == GLSL_SAMPLER_DIM_CUBE && t.is_array) {
            /* ir3 SAMGQ places dPdx/dPdy at src0[4], which collides with the
             * 5th src0 slot (3 cube coords + comparator + array layer).
             */
            air_note(c, "air.%s: shadow cube-array explicit gradient lowered to lod 0",
                     n->part[0]);
            op = nir_texop_txl;
            nir_def *zero_lod = nir_imm_floatN_t(&c->b, 0.0, coord->bit_size);
            if (!min_lod_is_zero)
               zero_lod = nir_fmax(&c->b, zero_lod,
                                   nir_f2fN(&c->b, min_lod, coord->bit_size));
            srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_lod,
                                          .src = nir_src_for_ssa(zero_lod) };
         } else {
            op = nir_texop_txd;
            srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_ddx,
                                          .src = nir_src_for_ssa(ddx) };
            srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_ddy,
                                          .src = nir_src_for_ssa(ddy) };
            if (!min_lod_is_zero)
               air_note(c, "air.%s: per-sample min_lod_clamp omitted (ir3 clamps LOD in "
                           "sampler state)", n->part[0]);
         }
      } else {
         if (level->bit_size != coord->bit_size)
            level = nir_f2fN(&c->b, level, coord->bit_size);
         bool level_is_zero = nir_src_is_const(nir_src_for_ssa(level)) &&
                              nir_src_as_float(nir_src_for_ssa(level)) == 0.0;
         if (is_level) {
            op = nir_texop_txl;
            if (!min_lod_is_zero)
               level = nir_fmax(&c->b, level, nir_f2fN(&c->b, min_lod, level->bit_size));
            srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_lod,
                                          .src = nir_src_for_ssa(level) };
         } else if (!level_is_zero) {
            op = nir_texop_txb;
            srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_bias,
                                          .src = nir_src_for_ssa(level) };
            if (!min_lod_is_zero)
               air_note(c, "air.%s: per-sample min_lod_clamp omitted (ir3 clamps LOD in "
                           "sampler state)", n->part[0]);
         } else {
            op = nir_texop_tex;
            if (!min_lod_is_zero)
               air_note(c, "air.%s: per-sample min_lod_clamp omitted (ir3 clamps LOD in "
                           "sampler state)", n->part[0]);
         }
      }
   } else if (!strcmp(t.op, "gather")) {
      uint64_t has_offset = 0;
      nir_def *offset = NULL;
      if (!t.no_offset && arg < in->num_operands &&
          in->operands[arg]->type->kind == AIR_TYPE_INT &&
          in->operands[arg]->type->bits == 1) {
         if (!tex_flag(c, in, arg, &has_offset))
            return false;
         arg++;
         if (in->num_operands <= arg || !(offset = air_get_ssa(c, in->operands[arg++])))
            return in->num_operands <= arg
                      ? air_error(c, "intrinsic: air.%s missing offset", n->part[0])
                      : false;
      }
      if (offset && has_offset && !all_zero(offset)) {
         if (offset->num_components != grad_comps)
            return air_error(c, "intrinsic: air.%s with a %u-component offset, wanting %u",
                             n->part[0], offset->num_components, grad_comps);
         srcs[nsrc++] = (nir_tex_src){ .src_type = nir_tex_src_offset,
                                       .src = nir_src_for_ssa(nir_i2i32(&c->b, offset)) };
      }
      op = nir_texop_tg4;
   } else if (!strcmp(t.op, "lod")) {
      op = nir_texop_lod;
   } else {
      /* read: a fetch by integer coordinate, with a level or a sample on
       * mipmapped / multisampled textures and level 0 on texture buffers.
       */
      nir_def *which;
      if (t.is_buffer || in->num_operands <= arg) {
         which = nir_imm_int(&c->b, 0);
      } else {
         which = air_get_ssa(c, in->operands[arg++]);
         if (!which)
            return false;
         which = nir_u2u32(&c->b, which);
      }
      op = t.is_ms ? nir_texop_txf_ms : nir_texop_txf;
      srcs[nsrc++] = (nir_tex_src){
         .src_type = t.is_ms ? nir_tex_src_ms_index : nir_tex_src_lod,
         .src = nir_src_for_ssa(which),
      };
   }

   unsigned samp_binding = have_sampler ? samp.binding : 0;
   nir_def *res = NULL;
   if (op == nir_texop_tg4 && !t.depth && arg < in->num_operands) {
      uint64_t component = 0;
      if (air_const_uint(in->operands[arg], &component)) {
         if (component >= 4)
            return air_error(c, "intrinsic: air.%s with component %u",
                             n->part[0], (unsigned)component);
         res = emit_single_tex(c, op, &t, dest_type, dest_bits, want_coord,
                               tex.binding, samp_binding, (unsigned)component,
                               srcs, nsrc);
      } else {
         nir_def *comp_ssa = air_get_ssa(c, in->operands[arg]);
         if (!comp_ssa)
            return false;
         comp_ssa = nir_iand_imm(&c->b, nir_u2u32(&c->b, comp_ssa), 3);
         nir_def *g[4];
         for (unsigned k = 0; k < 4; k++)
            g[k] = emit_single_tex(c, op, &t, dest_type, dest_bits, want_coord,
                                   tex.binding, samp_binding, k, srcs, nsrc);
         nir_def *g01 = nir_bcsel(&c->b, nir_ieq_imm(&c->b, comp_ssa, 1), g[1], g[0]);
         nir_def *g23 = nir_bcsel(&c->b, nir_ieq_imm(&c->b, comp_ssa, 3), g[3], g[2]);
         res = nir_bcsel(&c->b, nir_uge_imm(&c->b, comp_ssa, 2), g23, g01);
      }
   } else {
      res = emit_single_tex(c, op, &t, dest_type, dest_bits, want_coord,
                            tex.binding, samp_binding, 0, srcs, nsrc);
   }

   const struct air_type *value_type = in->base.type;
   if (value_type->kind == AIR_TYPE_STRUCT && value_type->num_members)
      value_type = value_type->members[0];

   unsigned nir_comps = res->num_components;
   unsigned want = air_nir_components(value_type);
   if (op == nir_texop_lod && t.unclamped_lod && nir_comps >= 2 && want == 1) {
      res = nir_channel(&c->b, res, 1);
   } else if (want < nir_comps) {
      res = nir_trim_vector(&c->b, res, want);
   } else if (want > nir_comps && t.compare && nir_comps == 1 && want == 4) {
      nir_def *z = nir_imm_floatN_t(&c->b, 0.0, dest_bits);
      nir_def *o = nir_imm_floatN_t(&c->b, 1.0, dest_bits);
      res = nir_vec4(&c->b, res, z, z, o);
   } else if (want > nir_comps) {
      return air_error(c, "intrinsic: air.%s gives %u components, the shader wants %u",
                       n->part[0], nir_comps, want);
   }
   set_tex_result(c, in, res);
   return true;
}

/* -- the dispatcher ----------------------------------------------------- */

bool
air_translate_call(struct air_ctx *c, const struct air_inst *in, const char *name)
{
   if (!strncmp(name, "llvm.", 5))
      return llvm_op(c, in, name);

   if (strncmp(name, "air.", 4))
      return air_call_in_module(c, in, name);

   struct air_name n;
   if (!split_name(name, &n))
      return air_error(c, "intrinsic: %s (a name this cannot take apart)", name);

   if (!strcmp(n.part[0], "is_function_constant_defined"))
      return fc_defined_op(c, in);
   if (!strncmp(n.part[0], "get_null_texture_", 17) ||
       !strncmp(n.part[0], "get_null_depth_", 15))
      return null_tex_op(c, in, &n);
   if (n.num >= 2 && !strcmp(n.part[1], "barrier") &&
       (!strcmp(n.part[0], "wg") || !strcmp(n.part[0], "simdgroup")))
      return barrier_op(c, in, !strcmp(n.part[0], "simdgroup"));
   if (!strcmp(n.part[0], "atomic"))
      return atomic_op(c, in, &n);
   if (!strcmp(n.part[0], "discard_fragment"))
      return discard_op(c, in);

   bool handled = false;
   /* before math_op, which resolves every argument as a number: these take a
    * lane number or nothing at all, and air.simd_sum falls through to it
    */
   if (!simd_op(c, in, &n, &handled))
      return false;
   if (handled)
      return true;

   if (!math_op(c, in, &n, &handled))
      return false;
   if (handled)
      return true;

   const char *op = n.part[0];
   if (strstr(op, "texture") || strstr(op, "_depth_") || !strncmp(op, "sample", 6) ||
       !strncmp(op, "gather", 6) || !strncmp(op, "read_", 5) || !strncmp(op, "write_", 6) ||
       strstr(op, "_lod_"))
      return texture_op(c, in, &n);

   return air_error(c, "intrinsic: %s", name);
}
