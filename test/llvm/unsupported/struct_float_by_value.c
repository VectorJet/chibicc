// EXPECT-ERROR: unsupported struct with floating-point members passed or returned by value
struct V {
  int n;
  double d;
};

int count(struct V v) {
  return v.n;
}

int main() {
  return 0;
}
