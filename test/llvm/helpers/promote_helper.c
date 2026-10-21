#include <stdarg.h>

// Built by the host C compiler and linked into variadic_promote.c.
// Each trailing argument is read as int, so the caller must have promoted
// narrow integers, as C requires for variadic arguments.
int check(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int a = va_arg(ap, int);
  int b = va_arg(ap, int);
  int c = va_arg(ap, int);
  va_end(ap);
  return (a == -1 && b == 255 && c == -2) ? 0 : 1;
}
