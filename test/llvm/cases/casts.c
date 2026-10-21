// EXPECT-EXIT: 3
int main() {
  unsigned char u = 200;
  signed char c = u;
  return (c == -56) + ((short)70000 == 4464) * 2;
}
