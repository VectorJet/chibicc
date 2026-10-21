// EXPECT-ERROR: unsupported struct or union member access
struct S {
  int a;
};

int main() {
  struct S s;
  s.a = 1;
  return s.a;
}
