// EXPECT-EXIT: 0
struct Flags {
  unsigned a : 3;
  int b : 5;
  _Bool c : 1;
  unsigned long d : 40;
  short : 0;
  char e : 4;
};

struct Flags global_flags = {5, -3, 1, 0x123456789, 7};

int main() {
  if (global_flags.a != 5 || global_flags.b != -3 || global_flags.c != 1)
    return 1;
  if (global_flags.d != 0x123456789 || global_flags.e != 7)
    return 2;

  struct Flags f = {0};
  f.a = 9;          // truncated to 1
  f.b = 15;
  f.c = 1;
  f.d = 0xFFFFFFFFFFUL;
  f.e = -2;
  if (f.a != 1 || f.b != 15 || f.c != 1 || f.d != 0xFFFFFFFFFFUL || f.e != -2)
    return 3;

  f.b += 1;         // 16 wraps to -16 in a signed 5-bit field
  if (f.b != -16)
    return 4;
  f.a++;
  f.a--;
  f.a--;
  if (f.a != 0)
    return 5;
  if ((f.a = 12) != 4)  // value of an assignment is the stored value
    return 6;
  f.d += 1;
  if (f.d != 0 || f.b != -16 || f.e != -2)
    return 7;
  return 0;
}
