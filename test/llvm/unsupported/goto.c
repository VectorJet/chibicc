// EXPECT-ERROR: unsupported goto
int main() {
  goto done;
  return 1;
done:
  return 0;
}
