// EXPECT-ERROR: of type array
int puts(char *s);

int main() {
  puts("hello");
  return 0;
}
