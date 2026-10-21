// EXPECT-EXIT: 0
int classify(int x) {
  int r = 0;
  switch (x) {
  case 0:
    return 10;
  case 1:
  case 2:
    r = 20;
    break;
  case 3 ... 5:
    r = 30;
    // fallthrough
  case 6:
    r += 1;
    break;
  case -1:
    return 99;
  default:
    r = 40;
  }
  return r;
}

int on_char(signed char c) {
  switch (c) {
  case 'a':
    return 1;
  case -2:
    return 2;
  }
  return 0;
}

long on_long(long v) {
  switch (v) {
  case 5000000000L - 4294967296L:
    return 1;
  }
  return 0;
}

int nested_break(int x) {
  int n = 0;
  for (int i = 0; i < 4; i++) {
    switch (i) {
    case 1:
      continue;
    case 3:
      break;
    default:
      n += x;
    }
    n++;
  }
  return n;
}

int no_body_case(int x) {
  switch (x)
    ;
  switch (x) {
  default:;
  }
  return 7;
}

int main() {
  if (classify(0) != 10 || classify(1) != 20 || classify(2) != 20)
    return 1;
  if (classify(3) != 31 || classify(5) != 31 || classify(6) != 1)
    return 2;
  if (classify(-1) != 99 || classify(7) != 40 || classify(-5) != 40)
    return 3;
  if (on_char('a') != 1 || on_char(-2) != 2 || on_char(3) != 0)
    return 4;
  if (on_long(705032704) != 1 || on_long(5000000000L) != 0)
    return 5;
  if (nested_break(10) != 23)
    return 6;
  if (no_body_case(3) != 7)
    return 7;
  return 0;
}
