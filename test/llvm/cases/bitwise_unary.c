// EXPECT-EXIT: 11
int main() {
  int a = -3;
  return ((6 & 3) | ((1 << 3) ^ 2)) + (-(-4)) + !0 + (~a - 2) * 0 + (-16 >> 2);
}
