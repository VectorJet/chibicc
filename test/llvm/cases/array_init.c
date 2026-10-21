// EXPECT-EXIT: 22
int main() {
  int a[5] = {1, 2, 3};
  char b[4] = "abc";
  short m[2][3] = {{1, 2, 3}, {4, 5, 6}};
  // 1+2+3+0+0, 'c' - 'a', m[1][2] + *m[1], then three checks worth 1, 1 and 2
  int r = a[0] + a[1] + a[2] + a[3] + a[4];
  r += b[2] - b[0];
  r += m[1][2] + *m[1];
  r += b[3] == 0;
  r += sizeof(m) == 12;
  r += sizeof(m[0]) / sizeof(m[0][0]) - 1;
  return r;
}
