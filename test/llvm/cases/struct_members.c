// EXPECT-EXIT: 0
struct Point { int x, y; };
struct Rect { struct Point min, max; char tag; };
struct Node { struct Node *next; long value; };
struct Anon {
  int kind;
  union {
    int i;
    struct { short lo, hi; };
  };
};

int area(struct Rect *r) {
  return (r->max.x - r->min.x) * (r->max.y - r->min.y);
}

struct Point origin = {0, 0};
struct Rect unit = {{0, 0}, {1, 1}, 'u'};
struct Point *corner = &unit.max;
struct Node n3 = {0, 3};
struct Node n2 = {&n3, 2};
struct Node n1 = {&n2, 1};

int main() {
  struct Rect r;
  r.min.x = 1;
  r.min.y = 2;
  r.max = r.min;
  r.max.x += 4;
  r.max.y++;
  if (area(&r) != 4 || sizeof(r) != 20)
    return 1;
  if (unit.tag != 'u' || corner->x != 1 || area(&unit) != 1)
    return 2;

  long sum = 0;
  for (struct Node *n = &n1; n; n = n->next)
    sum += n->value;
  if (sum != 6)
    return 3;

  struct Point pts[3] = {{1, 2}, {3, 4}};
  struct Point *p = pts + 1;
  if (p->y != 4 || pts[2].x != 0 || (&pts[1])[1].y != 0)
    return 4;

  struct Anon a;
  a.kind = 1;
  a.lo = 0x1234;
  a.hi = 0x5678;
  if (a.i != 0x56781234)
    return 5;

  struct Point q = {7, 8}, s = {9, 10};
  struct Point t = (q.x > 5 ? q : s);
  if (t.y != 8 || (0 ? q : s).x != 9)
    return 6;

  struct Point u, v;
  u = v = q;
  if (u.x != 7 || v.y != 8 || (u = s).x != 9)
    return 7;

  struct Point *cl = &(struct Point){11, 12};
  if (cl->x + cl->y != 23)
    return 8;

  struct { char c; } small1 = {1}, small2 = {2};
  if ((1 ? small1 : small2).c != 1)
    return 9;
  return 0;
}
