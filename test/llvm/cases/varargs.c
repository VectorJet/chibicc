// EXPECT-EXIT: 0
// Variadic function definitions: va_start, va_arg of integers, pointers,
// doubles and structs (including ones passed in memory), va_copy, and
// forwarding a va_list to libc.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct S12 { int a, b, c; };
struct S24 { long a, b, c; };

static long sum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  long r = 0;
  for (int i = 0; i < n; i++)
    r += va_arg(ap, int);
  va_end(ap);
  return r;
}

static double mixed(int n, ...) {
  va_list ap, again;
  va_start(ap, n);
  va_copy(again, ap);
  double r = 0;
  for (int i = 0; i < n; i++) {
    char *s = va_arg(ap, char *);
    long l = va_arg(ap, long);
    double d = va_arg(ap, double);
    struct S12 a = va_arg(ap, struct S12);
    struct S24 b = va_arg(ap, struct S24);
    r += strlen(s) + l + d + a.a + a.b + a.c + b.a + b.b + b.c;
  }
  r += va_arg(again, char *)[0];
  va_end(again);
  va_end(ap);
  return r;
}

static char buf[64];

static char *fmt(char *f, ...) {
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

int main() {
  // More arguments than argument registers.
  if (sum(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 55)
    return 1;
  struct S12 a = {1, 2, 3};
  struct S24 b = {10, 20, 30};
  double r = mixed(3, "ab", 100L, 0.5, a, b, "cde", 200L, 0.25, a, b, "f", 300L, 0.25, a, b);
  if (r != 6 + 600 + 1 + 3 * 66 + 'a')
    return 2;
  if (strcmp(fmt("%s-%d-%ld-%.2f", "x", 42, 7L, 2.5), "x-42-7-2.50"))
    return 3;
  return 0;
}
