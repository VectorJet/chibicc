// EXPECT-EXIT: 21
int g;

int bump() {
  g = g + 1;
  return 1;
}

int main() {
  int r = (0 && bump()) + (1 || bump()) + (1 && bump());
  return r * 10 + g;
}
