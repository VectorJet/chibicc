// EXPECT-EXIT: 0
// Frontend fixes needed for real-world code (found compiling sigil/QBE).

// An empty macro expansion at the end of a line must not hide a directive on
// the next line.
#define EMPTY(x)
int before EMPTY(0)
#define AFTER 5
;

// Unnamed parameters of function type, as in `void (char *)`.
int call_each(int (int), int (*)(int), int x);
static int inc(int x) { return x + 1; }
int call_each(int f(int), int (*g)(int), int x) { return f(x) + g(x); }

// A function-pointer parameter declared while the struct is incomplete.
typedef struct Ref Ref;
struct Target {
  int (*get)(Ref);
};
struct Ref {
  unsigned type : 3;
  unsigned val : 29;
};
static int get_val(Ref r) { return r.val; }

// Arrays of unknown size defined later.
extern int later[];
int later[2] = {3, 4};

int main() {
  if (AFTER != 5 || call_each(inc, inc, 1) != 4)
    return 1;

  // Postfix operators apply to compound literals.
  int b = 6;
  if ((char[4]){1, 0, 3, 2}[b & 3] != 3 || (struct Ref){1, 9}.val != 9)
    return 2;

  struct Target t = {get_val};
  Ref r = {2, 1000};
  if (t.get(r) != 1000 || later[1] != 4)
    return 3;

  // __int128.
  unsigned __int128 big = (unsigned __int128)1 << 100;
  __int128 neg = -(__int128)big;
  if ((long)(big >> 90) != 1024 || neg >= 0 || sizeof(__int128) != 16)
    return 4;
  __uint128_t product = (__uint128_t)0xFFFFFFFFFFFFFFFFUL * 0xFFFFFFFFFFFFFFFFUL;
  if ((unsigned long)(product >> 64) != 0xFFFFFFFFFFFFFFFEUL)
    return 5;

  // Plain char has the target's signedness; signed char is always signed.
  char c = -1;
  signed char sc = -1;
#ifdef __CHAR_UNSIGNED__
  if (c != 255 || '\xff' != 255)
    return 6;
#else
  if (c != -1 || '\xff' != -1)
    return 6;
#endif
  if (sc != -1)
    return 7;
  return 0;
}
