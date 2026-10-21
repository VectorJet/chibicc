// EXPECT-EXIT: 27
long diff(int *a, int *b) { return a - b; }

int main() {
  int a[8];
  int *p = a + 6;
  int *q = &a[2];
  p -= 1;
  q++;
  *(p - 1) = 7;
  int r = diff(p, q);      // 5 - 3 = 2
  r += a[4];               // 7
  r += (p > q) + (q >= a) + (p != q); // 3
  long *lp = (long *)a;
  r += (char *)(lp + 1) - (char *)lp; // 8
  r += 2[a + 2] == 7;      // 1
  unsigned u = 3;
  r += *(a + 1 + u) == 7;  // 1
  int *z = a + 8;
  r += z - a - 3;          // 5
  return r;
}
