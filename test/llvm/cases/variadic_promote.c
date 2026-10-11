// EXPECT-EXIT: 0
// LINK: promote_helper.c
int check(int n, ...);

int main() {
  char c = -1;
  unsigned char u = 255;
  short s = -2;
  return check(3, c, u, s);
}
