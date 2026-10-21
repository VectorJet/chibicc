// EXPECT-EXIT: 0
int *none() {
  return 0;
}

int main() {
  int *p = 0;
  if (none() != 0)
    return 1;
  return p == 0 ? 0 : 1;
}
