// EXPECT-EXIT: 0
int strcmp(char *a, char *b);

char *greeting = "hello";
char *names[] = {"zero", "one", "two"};
int values[3] = {5, 6, 7};
int *middle = &values[1];
int *end = values + 3;
long pad_and_ptr[3] = {9, (long)0, 11};
static int self_ref_target;
int *self_ref = &self_ref_target;
void *cyclic = &cyclic;
int answer(void) { return 42; }
int (*answer_ptr)(void) = answer;

int main() {
  if (strcmp(greeting, "hello"))
    return 1;
  if (strcmp(names[2], "two") || names[1][0] != 'o')
    return 2;
  if (*middle != 6 || end - values != 3)
    return 3;
  if (pad_and_ptr[0] + pad_and_ptr[2] != 20)
    return 4;
  *self_ref = 3;
  if (self_ref_target != 3)
    return 5;
  if (cyclic != &cyclic)
    return 6;
  if (answer_ptr == 0)
    return 7;
  return 0;
}
