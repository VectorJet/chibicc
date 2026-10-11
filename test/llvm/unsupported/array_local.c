// EXPECT-ERROR: unsupported pointer arithmetic
int main() {
  int a[3];
  a[0] = 1;
  return a[0];
}
