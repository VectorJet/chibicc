// EXPECT-EXIT: 10
int main() {
  int x = 5;
  return x > 3 ? x * 2 : x - 1;
}
