// EXPECT-ERROR: unsupported
int main() {
  void *target = &&out;
  goto *target;
out:
  return 0;
}
