// EXPECT-ERROR: unsupported statement 'switch'
int main() {
  switch (1) {
  case 1:
    return 3;
  }
  return 0;
}
