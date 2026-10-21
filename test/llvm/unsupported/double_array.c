// EXPECT-ERROR: unsupported comparison of non-scalar values
double d[2];

int main() {
  return d[1] > 0;
}
