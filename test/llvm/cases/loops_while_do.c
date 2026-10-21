// EXPECT-EXIT: 11
int main() {
  int k = 0;
  while (k < 5)
    k++;
  int d = 0;
  do {
    d += 2;
  } while (d < 6);
  return k + d;
}
