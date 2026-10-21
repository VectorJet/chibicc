// EXPECT-EXIT: 45
int main() {
  int a[10];
  for (int i = 0; i < 10; i++)
    a[i] = i;
  int sum = 0;
  for (int *p = a; p < a + 10; p++)
    sum += *p;
  return sum;
}
