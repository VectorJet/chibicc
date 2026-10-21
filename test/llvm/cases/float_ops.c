// EXPECT-EXIT: 0
// Floating-point arithmetic, comparisons, conversions, constants and globals.
float gf = 1.5f;
double gd = -2.25;
long double gld = 3.5L;
double table[3] = {0.5, 1.5, 2.5};
struct { float f; double d; } gs = {0.25f, 0.125};

static double half(double x) { return x / 2; }
static float addf(float a, float b) { return a + b; }
static long double twice_ld(long double x) { return x * 2; }

int main() {
  double d = 10;
  float f = 3;
  if (d / 4 != 2.5 || f * f != 9.0f || d - f != 7)
    return 1;
  if (-d != -10.0 || half(d) != 5.0 || addf(1.25f, 2.5f) != 3.75f)
    return 2;

  // Conversions.
  int i = 7.9;
  unsigned u = 4000000000.0;
  long l = -3.99;
  double from_int = -5, from_uint = 4000000000U, from_long = 1L << 40;
  if (i != 7 || u != 4000000000U || l != -3)
    return 3;
  if (from_int != -5.0 || from_uint != 4e9 || from_long != 1099511627776.0)
    return 4;
  float narrowed = 0.1;
  double widened = narrowed;
  if (widened == 0.1 || (float)widened != narrowed)
    return 5;
  _Bool b = 0.5;
  if (b != 1 || (_Bool)0.0 != 0)
    return 6;

  // Comparisons, including NaN.
  double zero = 0, nan = zero / zero;
  if (nan == nan || !(nan != nan) || nan < 1 || nan >= 1)
    return 7;
  if (!(1.5 < 2) || !(2.0 <= 2) || 3.0 > 4 || !nan)
    return 8;
  if (!(d > 9.99 && d >= 10 && f < 3.01f))
    return 9;

  // Globals, compound assignment, ++, ?:.
  gf += 1;
  gd *= -2;
  if (gf != 2.5f || gd != 4.5 || table[1] + table[2] != 4.0)
    return 10;
  if (gs.f + gs.d != 0.375)
    return 11;
  double x = 1.5;
  x++;
  x -= 0.25;
  if (x != 2.25 || (x > 2 ? x : -x) != 2.25)
    return 12;

  // long double.
  long double ld = gld;
  ld = twice_ld(ld) + 1;
  if (ld != 8.0L || (double)ld != 8.0 || (int)ld != 8)
    return 13;
  if (sizeof(long double) != 16 || _Alignof(long double) != 16)
    return 14;
  return 0;
}
