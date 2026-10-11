// EXPECT-EXIT: 12
int counter = 7;
int zero;
static int hidden = 2;

int advance() {
  counter = counter + hidden;
  return counter;
}

int main() {
  zero += 1;
  advance();
  advance();
  return counter + zero;
}
