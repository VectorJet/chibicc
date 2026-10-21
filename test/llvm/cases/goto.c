// EXPECT-EXIT: 55
int main() {
  int i = 0, sum = 0;
  goto check;
loop:
  sum += i;
  i++;
check:
  if (i <= 10)
    goto loop;
  goto done;
  sum = 1000;
done:
  return sum;
}
