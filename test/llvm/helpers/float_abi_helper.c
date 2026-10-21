// Host-compiled side of cases/float_abi.c.
#include <stdarg.h>
#include "float_abi_types.h"

double h_scalars(float f, double d, int i, long double ld) { return f + d + i + (double)ld; }
struct F2 h_f2(float a) { struct F2 s = {a, a + 1}; return s; }
struct F3 h_f3(float a) { struct F3 s = {a, a + 1, a + 2}; return s; }
struct D2 h_d2(double a) { struct D2 s = {a, a * 2}; return s; }
struct D4 h_d4(double a) { struct D4 s = {a, a + 1, a + 2, a + 3}; return s; }
struct ID h_id(int i, double d) { struct ID s = {i, d}; return s; }
struct DI h_di(double d, long i) { struct DI s = {d, i}; return s; }
struct FI h_fi(float f, int i) { struct FI s = {f, i}; return s; }
struct LD h_ld(long double x) { struct LD s = {x}; return s; }

double h_sum(struct F2 a, struct F3 b, struct D2 c, struct D4 d, struct ID e, struct DI f,
             struct FI g, struct LD h) {
  return a.a + a.b + b.a + b.b + b.c + c.a + c.b + d.a + d.b + d.c + d.d + e.i + e.d + f.d +
         f.i + g.f + g.i + (double)h.x;
}

// Eight doubles use up the x86-64 SSE registers (and AAPCS64 vector
// registers), so the structs after them go to memory or the stack.
double h_fp_exhausted(double a, double b, double c, double d, double e, double f, double g,
                      double h, struct D2 s, struct ID t, float last) {
  return a + b + c + d + e + f + g + h + s.a + s.b + t.i + t.d + last;
}

double h_varargs(int n, ...) {
  va_list ap;
  va_start(ap, n);
  double r = 0;
  for (int i = 0; i < n; i++) {
    r += va_arg(ap, double);
    struct D2 a = va_arg(ap, struct D2);
    struct ID b = va_arg(ap, struct ID);
    struct F3 c = va_arg(ap, struct F3);
    long double ld = va_arg(ap, long double);
    r += a.a + a.b + b.i + b.d + c.a + c.b + c.c + (double)ld;
  }
  va_end(ap);
  return r;
}

// Calls back into chibicc-compiled code.
double c_varargs(int n, ...);
struct D4 c_d4(struct F3 a, struct ID b);
struct DI c_di(struct DI x);

double h_callbacks(void) {
  struct D2 a = {1, 2};
  struct ID b = {3, 4};
  struct F3 c = {5, 6, 7};
  double r = c_varargs(2, 0.5, a, b, c, (long double)8, 0.25, a, b, c, (long double)8);
  struct D4 d = c_d4(c, b);
  struct DI e = c_di((struct DI){1.5, 2});
  return r + d.a + d.b + d.c + d.d + e.d + e.i;
}
