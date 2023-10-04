/*! \file fixedpoint.h
 *
 * Declarations for a fixed point real number library implemented on 32 bit
 * intergers.
 */

#ifndef THREADS_FIXEDPOINT_H
#define THREADS_FIXEDPOINT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*! Type to stored fixed point reals. */
typedef struct fp {int32_t v;} fp_val;

/*! Conversions between integers and fixed point values. */
fp_val fp_make(size_t point, long n);
long fp_trunc(size_t point, fp_val x);
long fp_round(size_t point, fp_val x);

/*! Fixed point arithmetic. */
fp_val fp_add(size_t point, fp_val x, fp_val y);
fp_val fp_sub(size_t point, fp_val x, fp_val y);
fp_val fp_mul(size_t point, fp_val x, fp_val y);
fp_val fp_div(size_t point, fp_val x, fp_val y);
fp_val fp_infix(size_t point, fp_val x, char op, fp_val y);

/*! Operations with integers */
fp_val fp_iadd(size_t point, fp_val x, long n);
fp_val fp_isub(size_t point, fp_val x, long n);
fp_val fp_irsub(size_t point, long n, fp_val x);
fp_val fp_imul(size_t point, fp_val x, long n);
fp_val fp_idiv(size_t point, fp_val x, long n);
fp_val fp_irdiv(size_t point, long n, fp_val x);

/*! Default position of the fixed point. */
#define FP_POINT_DEFAULT 14

/*! identity function for `fp_val`s for _FP. */
fp_val _fp_id(size_t point, fp_val x);
/*! If x is an fp_val, does nothing, otherwise, converts it to an fp_val.
    For internal use. */
#define _FP(x) _Generic((x),fp_val:_fp_id, default:fp_make)(FP_POINT_DEFAULT, x)

/*! Macros which autopopulate point with FP_POINT_DEFAULT. Arguments are
    automatically ensured to be `fp_val`s and integer values are instead
    converted to `fp_val`s in arithmetic macros. */

#define FP FP_MAKE

#define FP_MAKE(n) fp_make(FP_POINT_DEFAULT, n)
#define FP_TRUNC(x) fp_trunc(FP_POINT_DEFAULT, x)
#define FP_ROUND(x) fp_round(FP_POINT_DEFAULT, x)

#define FP_ADD(x, y) fp_add(FP_POINT_DEFAULT, _FP(x), _FP(y))
#define FP_SUB(x, y) fp_sub(FP_POINT_DEFAULT, _FP(x), _FP(y))
#define FP_MUL(x, y) fp_mul(FP_POINT_DEFAULT, _FP(x), _FP(y))
#define FP_DIV(x, y) fp_div(FP_POINT_DEFAULT, _FP(x), _FP(y))
#define FP_INFIX(x, op, y) fp_infix(FP_POINT_DEFAULT, _FP(x), op, _FP(y))

#endif