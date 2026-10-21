// EXPECT-EXIT: 9
int main() {
  int t = 0;
  for (int i = 0; i < 10; i++) {
    if (i % 2 == 0)
      continue;
    if (i == 7)
      break;
    t += i;
  }
  return t;
}
