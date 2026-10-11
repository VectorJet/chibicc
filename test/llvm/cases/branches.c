// EXPECT-EXIT: 13
int classify(int x) {
  if (x < 0)
    return 1;
  else if (x == 0)
    return 2;
  else
    return 3;
}

int main() {
  int r = 0;
  if (classify(-5) == 1)
    r = r + 1;
  if (classify(0) == 2 && classify(7) == 3)
    r += 2;
  if (classify(-1) != 1 || 0)
    r += 100;
  if (!(classify(9) == 3))
    r += 50;
  r += (r > 2) ? 10 : 20;
  return r;
}
