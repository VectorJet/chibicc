// EXPECT-ERROR: of type atomic
_Atomic int counter;

int main() {
  counter = 1;
  return counter;
}
