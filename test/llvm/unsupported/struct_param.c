// EXPECT-ERROR: unsupported parameter type struct
struct S {
  int a;
};

int get(struct S s) {
  return s.a;
}

int main() {
  return 0;
}
