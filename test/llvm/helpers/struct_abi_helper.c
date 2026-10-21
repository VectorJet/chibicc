// Host-compiled side of cases/struct_abi.c.
#include <stdarg.h>
#include "struct_abi_types.h"

struct S1 h_make1(char a) { struct S1 s = {a}; return s; }
struct S3 h_make3(char a, char b, char c) { struct S3 s = {a, b, c}; return s; }
struct S8 h_make8(int a, int b) { struct S8 s = {a, b}; return s; }
struct S12 h_make12(int a, int b, int c) { struct S12 s = {a, b, c}; return s; }
struct S16 h_make16(long a, long b) { struct S16 s = {a, b}; return s; }
struct S24 h_make24(long a, long b, long c) { struct S24 s = {a, b, c}; return s; }
struct A16 h_makea16(long a) { struct A16 s = {a}; return s; }
struct P5 h_makep5(char c, int i) { struct P5 s = {c, i}; return s; }
union U8 h_makeu8(int i) { union U8 u; u.i = i; u.c[4] = 4; u.c[5] = 5; return u; }

long h_sum1(struct S1 s) { return s.a; }
long h_sum3(struct S3 s) { return s.a + s.b + s.c; }
long h_sum8(struct S8 s) { return s.a + s.b; }
long h_sum12(struct S12 s) { return s.a + s.b + s.c; }
long h_sum16(struct S16 s) { return s.a + s.b; }
long h_sum24(struct S24 s) { s.a = 0; return s.b + s.c; }
long h_suma16(int x, struct A16 s) { return x + s.a; }
long h_sump5(struct P5 s) { return s.c + s.i; }
long h_sumu8(union U8 u) { return u.i + u.c[4] + u.c[5]; }

// Five integer registers are used up, so the 16-byte struct no longer fits in
// registers on x86-64 and goes to memory, while `last` still takes a register.
long h_regs_exhausted(long a, long b, long c, long d, long e, struct S16 s, long last) {
  return a + b + c + d + e + s.a + s.b + last;
}

// Hidden return pointer plus a full set of register arguments.
struct S24 h_sret_many(long a, long b, long c, long d, long e, long f, struct S12 s) {
  struct S24 r = {a + b + c, d + e + f, s.a + s.b + s.c};
  return r;
}

long h_varargs(int n, ...) {
  va_list ap;
  va_start(ap, n);
  long r = 0;
  for (int i = 0; i < n; i++) {
    struct S12 s = va_arg(ap, struct S12);
    struct S24 t = va_arg(ap, struct S24);
    r += s.a + s.b + s.c + t.a + t.b + t.c;
  }
  va_end(ap);
  return r;
}

// Calls back into functions compiled by chibicc.
struct S12 c_make12(int a);
struct S24 c_make24(long a);
long c_sum16(struct S16 s);
long c_sum24(struct S24 s);
long c_mixed(char c, struct S3 s3, struct A16 a, struct P5 p, union U8 u);

long h_callbacks(void) {
  struct S12 a = c_make12(1);
  struct S24 b = c_make24(10);
  struct S16 c = {100, 200};
  struct S24 d = {1000, 2000, 3000};
  struct S3 s3 = {1, 2, 3};
  struct A16 x = {40};
  struct P5 p = {5, 50};
  union U8 u;
  u.i = 7;
  u.c[4] = 1;
  u.c[5] = 2;
  long r = a.a + a.b + a.c + b.a + b.b + b.c + c_sum16(c);
  r += c_sum24(d);
  if (d.a != 1000)
    return -1; // the callee must not modify the caller's object
  r += c_mixed(9, s3, x, p, u);
  return r;
}
