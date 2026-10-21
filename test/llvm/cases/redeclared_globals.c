// EXPECT-EXIT: 0
// One object can be declared several times at file scope. All declarations
// must denote the same storage.
int x;
int x = 5;
int y = 7;
int y;
extern int z;
int read_z(void) { return z; }
int z = 9;
extern char buf[];
char *buf_ptr = buf;
char buf[4] = "abc";
int t;
int t;

int main() {
  extern int w;
  if (x != 5 || y != 7 || read_z() != 9)
    return 1;
  if (buf_ptr != buf || buf_ptr[1] != 'b')
    return 2;
  t = 3;
  if (t != 3 || w != 11)
    return 3;
  return 0;
}

int w = 11;
