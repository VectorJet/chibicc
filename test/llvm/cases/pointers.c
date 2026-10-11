// EXPECT-EXIT: 8
int main() {
  int x = 5;
  int *p = &x;
  *p = *p + 1;
  *p += 2;
  return x;
}
