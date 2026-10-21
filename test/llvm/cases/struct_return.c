// EXPECT-EXIT: 0
// Struct and union parameters and return values between functions compiled
// by chibicc, including member access on a call result.
struct Small { char a, b, c; };
struct Pair { long a, b; };
struct Big { long v[5]; };
union Word { long l; char c[8]; };

struct Small small(int x) {
  struct Small s = {x, x + 1, x + 2};
  return s;
}

struct Pair swap(struct Pair p) {
  struct Pair q = {p.b, p.a};
  return q;
}

struct Big fill(long x) {
  struct Big b;
  for (int i = 0; i < 5; i++)
    b.v[i] = x + i;
  return b;
}

long big_sum(struct Big b, int scale) {
  long s = 0;
  for (int i = 0; i < 5; i++)
    s += b.v[i];
  b.v[0] = 0;
  return s * scale;
}

union Word word(long l) {
  union Word w;
  w.l = l;
  return w;
}

static struct Pair counter(void) {
  static long n;
  n++;
  struct Pair p = {n, -n};
  return p;
}

int main() {
  if (small(5).c != 7)
    return 1;
  struct Pair p = {1, 2};
  p = swap(p);
  if (p.a != 2 || p.b != 1 || swap(swap(p)).a != 2)
    return 2;
  struct Big b = fill(10);
  if (big_sum(b, 2) != 120 || b.v[0] != 10)
    return 3;
  if (fill(1).v[4] != 5)
    return 4;
  if (word(0x0102).c[1] != 1)
    return 5;
  counter();
  if (counter().a != 2 || counter().b != -3)
    return 6;
  long total = 0;
  for (int i = 0; i < 3; i++)
    total += fill(i).v[i];
  if (total != 6)
    return 7;
  return 0;
}
