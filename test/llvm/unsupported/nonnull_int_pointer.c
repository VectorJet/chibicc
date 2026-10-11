// EXPECT-ERROR: unsupported conversion from non-null integer to pointer
int main() {
  int *p = 5;
  return p == 0;
}
