// EXPECT-EXIT: 0
// Function designators as values, calls through pointers, and pointers in
// structs and globals.
struct Ops {
  int (*op)(int);
  long (*two)(long, long);
};

static int twice(int x) { return 2 * x; }
static long add(long a, long b) { return a + b; }
static int apply(int (*f)(int), int x) { return f(x) + (*f)(x); }

struct Ops global_ops = {twice, add};
int (*table[2])(int) = {twice, 0};

int main() {
  struct Ops o;
  o.op = twice;
  o.two = &add;
  int (*f)(int) = twice;
  if (o.op(4) != 8 || o.two(2, 3) != 5 || f(5) != 10 || (*f)(1) != 2)
    return 1;
  if (apply(twice, 3) != 12 || global_ops.two(40, 2) != 42 || table[0](7) != 14)
    return 2;
  if (f != twice || !f || table[1])
    return 3;
  int (*g)(int) = 1 ? twice : 0;
  if (g(1) != 2)
    return 4;
  return 0;
}
