// EXPECT-ERROR: of type atomic
int main() {
  _Atomic int x = 1;
  return x;
}
