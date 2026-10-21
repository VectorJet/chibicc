// EXPECT-EXIT: 33
int add3(int a, int b, int c) {
  return a + b + c;
}

int twice(int x) {
  return x * 2;
}

int sub2(int a, int b) {
  return a - b;
}

int main() {
  return twice(add3(4, 5, 1)) + add3(1, 2, 3) + sub2(10, 3);
}
