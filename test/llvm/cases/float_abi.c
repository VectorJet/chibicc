// EXPECT-EXIT: 0
// LINK: float_abi_helper.c
// Floating-point scalars and aggregates across the ABI boundary.
#include <stdarg.h>
#include "../helpers/float_abi_types.h"

double h_scalars(float f, double d, int i, long double ld);
struct F2 h_f2(float a);
struct F3 h_f3(float a);
struct D2 h_d2(double a);
struct D4 h_d4(double a);
struct ID h_id(int i, double d);
struct DI h_di(double d, long i);
struct FI h_fi(float f, int i);
struct LD h_ld(long double x);
double h_sum(struct F2 a, struct F3 b, struct D2 c, struct D4 d, struct ID e, struct DI f,
             struct FI g, struct LD h);
double h_fp_exhausted(double a, double b, double c, double d, double e, double f, double g,
                      double h, struct D2 s, struct ID t, float last);
double h_varargs(int n, ...);
double h_callbacks(void);

double c_varargs(int n, ...) {
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

struct D4 c_d4(struct F3 a, struct ID b) {
  struct D4 r = {a.a, a.b, a.c, b.i + b.d};
  return r;
}

struct DI c_di(struct DI x) {
  x.d *= 2;
  x.i *= 3;
  return x;
}

int main() {
  if (h_scalars(1.5f, 2.25, 3, 4.0L) != 10.75)
    return 1;
  struct F2 f2 = h_f2(1);
  struct F3 f3 = h_f3(3);
  struct D2 d2 = h_d2(1.5);
  struct D4 d4 = h_d4(10);
  struct ID id = h_id(7, 0.5);
  struct DI di = h_di(0.25, 9);
  struct FI fi = h_fi(2.5f, 11);
  struct LD ld = h_ld(6.0L);
  if (f2.a != 1 || f2.b != 2 || f3.c != 5 || d2.b != 3 || d4.d != 13)
    return 2;
  if (id.i != 7 || id.d != 0.5 || di.d != 0.25 || di.i != 9 || fi.f != 2.5f || fi.i != 11)
    return 3;
  if (ld.x != 6.0L)
    return 4;
  // 3 + 12 + 4.5 + 46 + 7.5 + 9.25 + 13.5 + 6
  if (h_sum(f2, f3, d2, d4, id, di, fi, ld) != 101.75)
    return 5;
  if (h_fp_exhausted(1, 2, 3, 4, 5, 6, 7, 8, d2, id, 0.5f) != 36 + 4.5 + 7.5 + 0.5)
    return 6;
  // Per round: 0.5 + 4.5 + 7.5 + 12 + 8 = 32.5
  if (h_varargs(2, 0.5, d2, id, f3, (long double)8, 0.5, d2, id, f3, (long double)8) != 65)
    return 7;
  // c_varargs: 2 * (x + 3 + 7 + 18 + 8) with x = 0.5, 0.25 -> 72.75;
  // c_d4: 5 + 6 + 7 + 7 = 25; c_di: 3 + 6 = 9
  if (h_callbacks() != 72.75 + 25 + 9)
    return 8;
  return 0;
}
