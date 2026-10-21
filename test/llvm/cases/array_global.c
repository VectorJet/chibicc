// EXPECT-EXIT: 42
int exported_array[4] = {1, 2, 3, 4};
static long table[3] = {10, 20};
char zeroed[16];

int main() {
  int r = 0;
  for (int i = 0; i < 4; i++)
    r += exported_array[i];
  r += table[0] + table[1] + table[2];
  for (int i = 0; i < 16; i++)
    r += zeroed[i];
  zeroed[15] = 2;
  return r + zeroed[15];
}
