// EXPECT-EXIT: 13
int main() {
  int *p = (int *)4096;
  long n = (long)p;
  int x = 0;
  unsigned long addr = (unsigned long)&x;
  int *back = (int *)addr;
  *back = 5;
  char *c = (char *)(n + 3);
  return (n == 4096) + (c - (char *)p) + x + ((p + 1) == (int *)4100) * 4;
}
