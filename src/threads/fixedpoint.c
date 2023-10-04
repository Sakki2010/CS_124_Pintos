#include <debug.h>

#include "fixedpoint.h"

/*! In all below, f refers to 2^p, where p is index of the fixed point, x,y are
    real numbers, and n is an integer.

    Behavior is undefined unless 0 < p < 32. */

/*! Wraps an int32_t in an fp_val struct. Internal helper. */
static inline fp_val fp(int32_t v) {
    return (fp_val) {.v = v};
}

/*! Convert n to a fixed point: n * f */
fp_val fp_make(size_t p, long n) {
    return fp(n << p);
}
/*! Convert x to integer (rounding toward zero): x / f */
long fp_trunc(size_t p, fp_val x) {
    if (x.v < 0) {
        return (x.v + (1 << p) - 1) >> p;
    } else {
        return x.v >> p;
    }
}
/*! Convert x to integer (rounding to nearest):
    (x + f / 2) / f if x >= 0,
    (x - f / 2) / f if x <= 0. */
long fp_round(size_t p, fp_val x) {
    return (x.v + (1 << (p - 1))) >> p;
}

/*! Add x and y: x + y */
fp_val fp_add(size_t p UNUSED, fp_val x, fp_val y) {
    return fp(x.v + y.v);
}
/*! Subtract y from x: x - y */
fp_val fp_sub(size_t p UNUSED, fp_val x, fp_val y) {
    return fp(x.v - y.v);
}
/*! Multiply x by y: ((int64_t) x) * y / f */
fp_val fp_mul(size_t p, fp_val x, fp_val y) {
    return fp(((int64_t) x.v) * y.v >> p);
}
/*! Divide x by y: ((int64_t) x) * f / y */
fp_val fp_div(size_t p, fp_val x, fp_val y) {
    return fp((((int64_t) x.v) << p) / y.v);
}
/*! Infix notation, dispatches the appropriate */
fp_val fp_infix(size_t p, fp_val x, char op, fp_val y) {
    switch (op) {
        case '+': return fp_add(p, x, y);
        case '-': return fp_sub(p, x, y);
        case '*': return fp_mul(p, x, y);
        case '/': return fp_div(p, x, y);
        default: PANIC("Invalid infix operator `%c'!\n", op);
    }
}

/*! Add x and n: x + n * f */
fp_val fp_iadd(size_t p, fp_val x, long n) {
    return fp(x.v + (n << p));
}
/*! Subtract n from x: x - n * f */
fp_val fp_isub(size_t p, fp_val x, long n) {
    return fp(x.v - (n << p));
}
/*! Subtract x from n: n * f - x*/
fp_val fp_irsub(size_t p, long n, fp_val x) {
    return fp((n << p) - x.v);
}
/*! Multiply x by n: x * n */
fp_val fp_imul(size_t p UNUSED, fp_val x, long n) {
    return fp(x.v * n);
}
/*! Divide x by n: x / n */
fp_val fp_idiv(size_t p UNUSED, fp_val x, long n) {
    return fp(x.v / n);
}
/*! Divide n by x: ((int64_t) n) * f * f / x */
fp_val fp_irdiv(size_t p, long n, fp_val x) {
    return fp_div(p, fp_make(p, n), x);
}

fp_val _fp_id(size_t point UNUSED, fp_val x) {
    return x;
}