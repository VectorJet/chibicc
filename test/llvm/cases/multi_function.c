// EXPECT-EXIT: 6
int fib(int n) {
  if (n < 2)
    return n;
  return fib(n - 1) + fib(n - 2);
}

int is_even(int n) {
  return n % 2 == 0;
}

int main() {
  return fib(10) - 50 + is_even(4) + is_even(7);
}
