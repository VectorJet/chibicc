// EXPECT-EXIT: 9
int g;

void set(int v) {
  g = v;
  return;
}

int main() {
  set(9);
  return g;
}
