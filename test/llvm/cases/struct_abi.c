// EXPECT-EXIT: 0
// LINK: struct_abi_helper.c
// Structs and unions by value across the boundary with host-compiled code.
#include "../helpers/struct_abi_types.h"

struct S1 h_make1(char a);
struct S3 h_make3(char a, char b, char c);
struct S8 h_make8(int a, int b);
struct S12 h_make12(int a, int b, int c);
struct S16 h_make16(long a, long b);
struct S24 h_make24(long a, long b, long c);
struct A16 h_makea16(long a);
struct P5 h_makep5(char c, int i);
union U8 h_makeu8(int i);
long h_sum1(struct S1 s);
long h_sum3(struct S3 s);
long h_sum8(struct S8 s);
long h_sum12(struct S12 s);
long h_sum16(struct S16 s);
long h_sum24(struct S24 s);
long h_suma16(int x, struct A16 s);
long h_sump5(struct P5 s);
long h_sumu8(union U8 u);
long h_regs_exhausted(long a, long b, long c, long d, long e, struct S16 s, long last);
struct S24 h_sret_many(long a, long b, long c, long d, long e, long f, struct S12 s);
long h_varargs(int n, ...);
long h_callbacks(void);

struct S12 c_make12(int a) {
  struct S12 s = {a, a + 1, a + 2};
  return s;
}

struct S24 c_make24(long a) {
  struct S24 s;
  s.a = a;
  s.b = a * 2;
  s.c = a * 3;
  return s;
}

long c_sum16(struct S16 s) { return s.a + s.b; }

long c_sum24(struct S24 s) {
  long r = s.a + s.b + s.c;
  s.a = 0; // modifies this call's copy only
  return r;
}

long c_mixed(char c, struct S3 s3, struct A16 a, struct P5 p, union U8 u) {
  return c + s3.a + s3.b + s3.c + a.a + p.c + p.i + u.i + u.c[4] + u.c[5];
}

int main() {
  if (h_make1(7).a != 7)
    return 1;
  struct S3 s3 = h_make3(1, 2, 3);
  if (s3.a != 1 || s3.b != 2 || s3.c != 3)
    return 2;
  struct S8 s8 = h_make8(4, 5);
  struct S12 s12 = h_make12(6, 7, 8);
  struct S16 s16 = h_make16(9, 10);
  struct S24 s24 = h_make24(11, 12, 13);
  if (s8.a + s8.b != 9 || s12.a + s12.b + s12.c != 21)
    return 3;
  if (s16.a + s16.b != 19 || s24.a + s24.b + s24.c != 36)
    return 4;
  struct A16 a16 = h_makea16(14);
  struct P5 p5 = h_makep5(15, 16);
  union U8 u8 = h_makeu8(17);
  if (a16.a != 14 || p5.c != 15 || p5.i != 16)
    return 5;
  if (u8.i != 17 || u8.c[4] != 4 || u8.c[5] != 5)
    return 6;

  if (h_sum1(h_make1(3)) != 3 || h_sum3(s3) != 6 || h_sum8(s8) != 9)
    return 7;
  if (h_sum12(s12) != 21 || h_sum16(s16) != 19)
    return 8;
  if (h_sum24(s24) != 25 || s24.a != 11)
    return 9;
  if (h_suma16(1, a16) != 15 || h_sump5(p5) != 31 || h_sumu8(u8) != 26)
    return 10;

  if (h_regs_exhausted(1, 2, 3, 4, 5, s16, 6) != 40)
    return 11;
  struct S24 r = h_sret_many(1, 2, 3, 4, 5, 6, s12);
  if (r.a != 6 || r.b != 15 || r.c != 21)
    return 12;
  if (h_varargs(2, s12, s24, s12, s24) != 2 * (21 + 36))
    return 13;

  // 1+2+3 + 10+20+30 + 300 + 6000 + (9+6+40+55+7+1+2)
  if (h_callbacks() != 6486)
    return 14;
  return 0;
}
