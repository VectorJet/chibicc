// EXPECT-EXIT: 0
struct P {
  char c;
  int i;
  long l;
} __attribute__((packed));

struct Outer {
  char pad;
  struct P p;
} __attribute__((packed));

int main() {
  struct Outer o;
  o.p.c = 1;
  o.p.i = 0x01020304;
  o.p.l = 0x1122334455667788L;
  if (sizeof(struct P) != 13 || sizeof(o) != 14)
    return 1;
  if (o.p.i != 0x01020304 || o.p.l != 0x1122334455667788L)
    return 2;
  char *raw = (char *)&o;
  if (raw[2] != 4 || raw[6] != (char)0x88)
    return 3;
  struct P copy = o.p;
  if (copy.l != o.p.l)
    return 4;
  return 0;
}
