// EXPECT-EXIT: 0
int strcmp(char *a, char *b);
int printf(char *fmt, ...);
int puts(char *s);

static int len(char *s) {
  int n = 0;
  while (*s++)
    n++;
  return n;
}

int main() {
  puts("hello");
  printf("%s %d\n", "world", 42);
  if (len("abcd") != 4)
    return 1;
  if (strcmp(__func__, "main"))
    return 2;
  if ("xyz"[1] != 'y')
    return 3;
  if (sizeof("abc") != 4)
    return 4;
  return 0;
}
