// EXPECT-EXIT: 14
int main() {
  int a = 3;
  int b;
  b = a * 4;
  a += b;
  a -= 1;
  return a;
}
