// sigIL backend (--backend=sigil): chibicc as sigcc, Sigma's C compiler
// (Sigma book 26.4.1-26.4.2). C is lowered into sigIL text, which flows
// through the same sigil backend as Sigma code.
//
// The lowering is deliberately plain:
//   - every local lives in an entry-block alloca;
//   - a pointer value is a ptr(u8) address, bitcast to ptr(T) at each load
//     or store (sigil types its memory accesses);
//   - a struct or union value is the address of its storage, copied in
//     chunks on assignment and passed or returned as a sigil aggregate;
//   - &&, || and ?: go through a slot instead of phi nodes.
// What sigIL cannot express (inline asm, defining variadic functions,
// long double, VLAs, labels as values, thread-local storage) is an error.

#include "chibicc.h"

bool opt_backend_sigil;
char *opt_sigil_prefix = "c.";

static FILE *out;
static FILE *fb;            // the current function's body
static char *fb_buf;
static size_t fb_len;
static FILE *ab;            // the current function's entry-block allocas
static char *ab_buf;
static size_t ab_len;
static FILE *tb;            // type definitions
static char *tb_buf;
static size_t tb_len;
static Obj *current_fn;
static int ntmp;
static int nlbl;
static bool term;           // the current block has its terminator
static int nvar;
static HashMap statics;     // names of file-local symbols (prefixed)

static char *gen_expr(Node *node);
static void gen_stmt(Node *node);
static char *gen_addr(Node *node);

// ── output ──────────────────────────────────────────────────────────────

static char *tmp(void) {
  return format("%%t%d", ++ntmp);
}

__attribute__((format(printf, 1, 2)))
static void emit(char *fmt, ...) {
  if (term) {
    // code after a terminator: an unreachable block of its own
    fprintf(fb, "@d%d:\n", ++nlbl);
    term = false;
  }
  va_list ap;
  va_start(ap, fmt);
  fprintf(fb, "    ");
  vfprintf(fb, fmt, ap);
  fprintf(fb, "\n");
  va_end(ap);
}

static char *new_label(void) {
  return format("L%d", ++nlbl);
}

// chibicc's labels (.L..12) as sigil block names
static char *lbl(char *name) {
  char *s = format("C%s", name);
  for (char *p = s; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '_')
      *p = '_';
  return s;
}

static void label(char *name) {
  if (!term)
    fprintf(fb, "    jmp @%s\n", name);
  fprintf(fb, "@%s:\n", name);
  term = false;
}

static void jmp(char *name) {
  emit("jmp @%s", name);
  term = true;
}

static void br(char *cond, char *t, char *f) {
  emit("jnz %s, @%s, @%s", cond, t, f);
  term = true;
}

// a symbol name for sigil: file-local ones get the prefix
static char *gsym(char *name) {
  char *s = strdup(name);
  for (char *p = s; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.')
      *p = '_';
  if (name[0] == '.' || hashmap_get(&statics, name))
    return format("%s%s", opt_sigil_prefix, s[0] == '.' ? s + 1 : s);
  return s;
}

// ── types ───────────────────────────────────────────────────────────────

static bool is_agg(Type *ty) {
  return ty->kind == TY_STRUCT || ty->kind == TY_UNION;
}

// a value's address stands for it: aggregates, arrays, functions
static bool by_addr(Type *ty) {
  return is_agg(ty) || ty->kind == TY_ARRAY || ty->kind == TY_FUNC || ty->kind == TY_VLA;
}

static bool is_ptrish(Type *ty) {
  return ty->kind == TY_PTR || by_addr(ty);
}

static noreturn void unsupported(Token *tok, char *what) {
  if (tok)
    error_tok(tok, "sigcc: %s is not supported by the sigIL backend", what);
  error("sigcc: %s is not supported by the sigIL backend", what);
}

// the sigil type of a scalar value
static char *vty(Type *ty) {
  switch (ty->kind) {
  case TY_VOID: return "void";
  case TY_BOOL: return "u8";
  case TY_CHAR: return ty->is_unsigned ? "u8" : "i8";
  case TY_SHORT: return ty->is_unsigned ? "u16" : "i16";
  case TY_INT:
  case TY_ENUM: return ty->is_unsigned ? "u32" : "i32";
  case TY_LONG: return ty->is_unsigned ? "u64" : "i64";
  case TY_INT128: return ty->is_unsigned ? "u128" : "i128";
  case TY_FLOAT: return "f32";
  case TY_DOUBLE: return "f64";
  case TY_LDOUBLE: unsupported(ty->name, "long double");
  default: return "ptr(u8)";
  }
}

typedef struct AggName AggName;
struct AggName {
  AggName *next;
  void *key;
  char *name;
};
static AggName *agg_names;
static int nagg;

static char *agg_type(Type *ty);

// a field's sigil type, or NULL when it has none
static char *fty(Type *ty) {
  switch (ty->kind) {
  case TY_ARRAY: {
    char *e = fty(ty->base);
    if (!e || ty->array_len <= 0)
      return NULL;
    return format("[%s x %d]", e, ty->array_len);
  }
  case TY_STRUCT:
  case TY_UNION:
    return agg_type(ty);
  case TY_LDOUBLE:
  case TY_VLA:
  case TY_FUNC:
  case TY_VOID:
    return NULL;
  default:
    return vty(ty);
  }
}

// does a struct have the layout sigil gives its fields in order?
static bool natural_layout(Type *ty) {
  if (ty->kind != TY_STRUCT || ty->is_packed || ty->is_flexible || !ty->members)
    return false;
  int off = 0;
  int maxa = 1;
  for (Member *m = ty->members; m; m = m->next) {
    if (m->is_bitfield || !fty(m->ty))
      return false;
    int a = m->ty->align;
    off = align_to(off, a);
    if (off != m->offset)
      return false;
    off += m->ty->size;
    if (a > maxa)
      maxa = a;
  }
  return align_to(off, maxa) == ty->size && maxa == ty->align;
}

// the sigil name of a struct / union type (defined on first use)
static char *agg_type(Type *ty) {
  void *key = ty->members ? (void *)ty->members : (void *)ty;
  for (AggName *a = agg_names; a; a = a->next)
    if (a->key == key)
      return a->name;
  AggName *a = calloc(1, sizeof(AggName));
  a->key = key;
  a->name = format("@%sT%d", opt_sigil_prefix, ++nagg);
  a->next = agg_names;
  agg_names = a;
  char *def;
  if (natural_layout(ty)) {
    char *buf;
    size_t len;
    FILE *f = open_memstream(&buf, &len);
    fprintf(f, "{ ");
    for (Member *m = ty->members; m; m = m->next)
      fprintf(f, "%s%s", fty(m->ty), m->next ? ", " : "");
    fprintf(f, " }");
    fclose(f);
    def = buf;
  } else {
    // unions, bitfields, packed layouts: opaque storage of the right
    // size and alignment
    int al = ty->align >= 8 ? 8 : ty->align >= 4 ? 4 : ty->align >= 2 ? 2 : 1;
    char *unit = al == 8 ? "i64" : al == 4 ? "i32" : al == 2 ? "i16" : "u8";
    int n = ty->size / al;
    if (n <= 0)
      n = 1;
    def = format("{ [%s x %d] }", unit, n);
  }
  fprintf(tb, "type %s = %s;\n", a->name, def);
  return a->name;
}

// the type of a parameter, argument or return value
static char *aty(Type *ty) {
  if (is_agg(ty))
    return agg_type(ty);
  return vty(ty);
}

// ── values ──────────────────────────────────────────────────────────────

static char *bitcast(char *v, char *from, char *to) {
  char *t = tmp();
  emit("%s = bitcast %s %s to %s", t, from, v, to);
  return t;
}

static char *typed_ptr(char *addr, char *t) {
  return bitcast(addr, "ptr(u8)", format("ptr(%s)", t));
}

static char *offset_ptr(char *addr, long n) {
  if (n == 0)
    return addr;
  char *t = tmp();
  emit("%s = offset ptr(u8) %s, i64 %ld", t, addr, n);
  return t;
}

static char *load_as(char *addr, char *t) {
  char *p = typed_ptr(addr, t);
  char *v = tmp();
  emit("%s = load %s, ptr(%s) %s", v, t, t, p);
  return v;
}

static void store_as(char *addr, char *t, char *v) {
  char *p = typed_ptr(addr, t);
  emit("store %s %s, ptr(%s) %s", t, v, t, p);
}

// copy size bytes from src to dst in the widest chunks the alignment allows
static void copy_mem(char *dst, char *src, int size, int align) {
  int pos = 0;
  int widths[] = {8, 4, 2, 1};
  char *names[] = {"i64", "i32", "i16", "u8"};
  for (int w = 0; w < 4; w++) {
    if (widths[w] > align && widths[w] != 1)
      continue;
    while (size - pos >= widths[w]) {
      char *v = load_as(offset_ptr(src, pos), names[w]);
      store_as(offset_ptr(dst, pos), names[w], v);
      pos += widths[w];
    }
  }
}

static void zero_mem(char *dst, int size, int align) {
  int pos = 0;
  int widths[] = {8, 4, 2, 1};
  char *names[] = {"i64", "i32", "i16", "u8"};
  for (int w = 0; w < 4; w++) {
    if (widths[w] > align && widths[w] != 1)
      continue;
    while (size - pos >= widths[w]) {
      store_as(offset_ptr(dst, pos), names[w], "0");
      pos += widths[w];
    }
  }
}

static char *load(Type *ty, char *addr) {
  if (by_addr(ty))
    return addr;
  return load_as(addr, vty(ty));
}

static void store(Type *ty, char *addr, char *v) {
  if (is_agg(ty)) {
    copy_mem(addr, v, ty->size, ty->align);
    return;
  }
  store_as(addr, vty(ty), v);
}

// a slot of the entry block holding one value of sigil type t
static char *slot(char *t) {
  char *m = format("%%s%d", ++ntmp);
  char *s = format("%%a%d", ntmp);
  fprintf(ab, "    %s = alloca %s\n", m, t);
  fprintf(ab, "    %s = bitcast ptr(%s) %s to ptr(u8)\n", s, t, m);
  return s;
}

static int int_size(Type *ty) {
  return ty->kind == TY_BOOL ? 1 : ty->size;
}

// an integer of type `from` as type `to`
static char *int_resize(char *v, Type *from, Type *to) {
  char *f = vty(from);
  char *t = vty(to);
  int sf = int_size(from);
  int st = int_size(to);
  if (!strcmp(f, t))
    return v;
  char *r = tmp();
  if (sf == st)
    emit("%s = bitcast %s %s to %s", r, f, v, t);
  else if (st > sf)
    emit("%s = %s %s %s to %s", r, from->is_unsigned || from->kind == TY_BOOL ? "zext" : "sext", f, v, t);
  else
    emit("%s = trunc %s %s to %s", r, f, v, t);
  return r;
}

// v != 0 as an i1
static char *to_i1(char *v, Type *ty) {
  char *r = tmp();
  if (is_ptrish(ty)) {
    char *i = bitcast(v, "ptr(u8)", "i64");
    emit("%s = cne i64 %s, 0", r, i);
  } else if (ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE) {
    emit("%s = cne %s %s, 0.0", r, vty(ty), v);
  } else {
    emit("%s = cne %s %s, 0", r, vty(ty), v);
  }
  return r;
}

static char *from_i1(char *c, Type *ty) {
  char *r = tmp();
  emit("%s = zext i1 %s to %s", r, c, vty(ty));
  return r;
}

static char *cond_i1(Node *node) {
  return to_i1(gen_expr(node), node->ty);
}

static char *conv(char *v, Type *from, Type *to) {
  if (to->kind == TY_VOID)
    return NULL;
  if (to->kind == TY_BOOL)
    return from_i1(to_i1(v, from), to);
  bool fp = is_ptrish(from), tp = is_ptrish(to);
  bool ff = is_flonum(from), tf = is_flonum(to);
  if (fp && tp)
    return v;
  if (by_addr(to) || by_addr(from))
    return v;
  if (fp) {
    char *i = bitcast(v, "ptr(u8)", "u64");
    return int_resize(i, ty_ulong, to);
  }
  if (tp) {
    if (ff)
      unsupported(NULL, "a float converted to a pointer");
    char *i = int_resize(v, from, from->is_unsigned ? ty_ulong : ty_long);
    return bitcast(i, vty(from->is_unsigned ? ty_ulong : ty_long), "ptr(u8)");
  }
  if (!ff && !tf)
    return int_resize(v, from, to);
  char *r = tmp();
  if (!ff && tf) {
    emit("%s = %s %s %s to %s", r, from->is_unsigned || from->kind == TY_BOOL ? "uitofp" : "sitofp", vty(from), v, vty(to));
    return r;
  }
  if (ff && !tf) {
    emit("%s = %s %s %s to %s", r, to->is_unsigned ? "fptoui" : "fptosi", vty(from), v, vty(to));
    return r;
  }
  if (from->kind == to->kind)
    return v;
  emit("%s = %s %s %s to %s", r, from->kind == TY_FLOAT ? "fext" : "ftrunc", vty(from), v, vty(to));
  return r;
}

// a float constant, exactly: its bits reinterpreted
static char *float_const(Type *ty, long double fv) {
  char *i = tmp();
  if (ty->kind == TY_FLOAT) {
    union { float f; uint32_t u; } u = {(float)fv};
    emit("%s = copy u32 %u", i, u.u);
    return bitcast(i, "u32", "f32");
  }
  union { double f; uint64_t u; } u = {(double)fv};
  emit("%s = copy u64 %lu", i, (unsigned long)u.u);
  return bitcast(i, "u64", "f64");
}

// ── bitfields ───────────────────────────────────────────────────────────

static char *bitfield_load(Member *mem, char *addr) {
  char *t = vty(mem->ty);
  char *v = load_as(addr, t);
  char *w = int_resize(v, mem->ty, mem->ty->is_unsigned ? ty_ulong : ty_long);
  char *wt = mem->ty->is_unsigned ? "u64" : "i64";
  char *a = tmp();
  emit("%s = shl %s %s, %d", a, wt, w, 64 - mem->bit_width - mem->bit_offset);
  char *b = tmp();
  emit("%s = %s %s %s, %d", b, mem->ty->is_unsigned ? "shr" : "sar", wt, a, 64 - mem->bit_width);
  return int_resize(b, mem->ty->is_unsigned ? ty_ulong : ty_long, mem->ty);
}

static void bitfield_store(Member *mem, char *addr, char *v, Type *vt) {
  char *t = vty(mem->ty);
  char *nv = int_resize(conv(v, vt, mem->ty), mem->ty, ty_ulong);
  unsigned long mask = (mem->bit_width >= 64 ? ~0UL : ((1UL << mem->bit_width) - 1)) << mem->bit_offset;
  char *a = tmp();
  emit("%s = shl u64 %s, %d", a, nv, mem->bit_offset);
  char *b = tmp();
  emit("%s = and u64 %s, %lu", b, a, mask);
  char *old = int_resize(load_as(addr, t), mem->ty, ty_ulong);
  char *c = tmp();
  emit("%s = and u64 %s, %lu", c, old, ~mask);
  char *d = tmp();
  emit("%s = or u64 %s, %s", d, c, b);
  store_as(addr, t, int_resize(d, ty_ulong, mem->ty));
}

// ── addresses ───────────────────────────────────────────────────────────

static char *var_addr(Obj *var) {
  if (var->is_local)
    return format("%%v%d", var->offset);
  if (var->is_tls)
    unsupported(var->tok, "thread-local storage");
  char *t = tmp();
  emit("%s = addr @%s", t, gsym(var->name));
  return t;
}

static char *gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    if (node->var->ty->kind == TY_VLA)
      unsupported(node->tok, "a variable-length array");
    return var_addr(node->var);
  case ND_DEREF:
    return gen_expr(node->lhs);
  case ND_COMMA:
    gen_expr(node->lhs);
    return gen_addr(node->rhs);
  case ND_MEMBER:
    return offset_ptr(gen_addr(node->lhs), node->member->offset);
  case ND_FUNCALL:
  case ND_ASSIGN:
  case ND_COND:
  case ND_STMT_EXPR:
    if (is_agg(node->ty))
      return gen_expr(node);
    break;
  case ND_VLA_PTR:
    unsupported(node->tok, "a variable-length array");
  default:
    break;
  }
  error_tok(node->tok, "not an lvalue");
}

// ── calls ───────────────────────────────────────────────────────────────

static char *gen_call(Node *node) {
  if (node->lhs->kind == ND_VAR && !strcmp(node->lhs->var->name, "alloca")) {
    char *n = conv(gen_expr(node->args), node->args->ty, ty_long);
    char *r = tmp();
    emit("%s = alloca u8, i64 %s", r, n);
    return r;
  }
  Type *fty = node->func_ty;
  int nparams = 0;
  for (Type *p = fty->params; p; p = p->next)
    nparams++;

  // the arguments, left to right
  int nargs = 0;
  for (Node *a = node->args; a; a = a->next)
    nargs++;
  char **vals = calloc(nargs + 1, sizeof(char *));
  char **tys = calloc(nargs + 1, sizeof(char *));
  int i = 0;
  for (Node *a = node->args; a; a = a->next, i++) {
    vals[i] = gen_expr(a);
    if (i >= nparams && is_flonum(a->ty))
      unsupported(a->tok, "a floating-point variadic argument");
    if (is_agg(a->ty)) {
      tys[i] = agg_type(a->ty);
      vals[i] = bitcast(vals[i], "ptr(u8)", format("ptr(%s)", tys[i]));
    } else {
      tys[i] = vty(a->ty);
    }
  }

  // the callee: a named function or a pointer
  char *callee;
  if (node->lhs->kind == ND_VAR && node->lhs->var->ty->kind == TY_FUNC) {
    callee = format("@%s", gsym(node->lhs->var->name));
  } else {
    char *fp = gen_expr(node->lhs);
    callee = format("ptr(func) %s", bitcast(fp, "ptr(u8)", "ptr(func)"));
  }

  char *buf;
  size_t len;
  FILE *f = open_memstream(&buf, &len);
  for (int k = 0; k < nargs; k++)
    fprintf(f, "%s%s %s", k ? ", " : "", tys[k], vals[k]);
  fclose(f);

  Type *rt = node->ty;
  if (rt->kind == TY_VOID) {
    emit("call void %s(%s)", callee, buf);
    return NULL;
  }
  char *r = tmp();
  emit("%s = call %s %s(%s)", r, aty(rt), callee, buf);
  if (is_agg(rt))
    return bitcast(r, format("ptr(%s)", agg_type(rt)), "ptr(u8)");
  return r;
}

// ── expressions ─────────────────────────────────────────────────────────

static char *cmp_op(NodeKind k, Type *ty) {
  bool u = ty->is_unsigned || is_ptrish(ty);
  switch (k) {
  case ND_EQ: return "ceq";
  case ND_NE: return "cne";
  case ND_LT: return u ? "cult" : "cslt";
  case ND_LE: return u ? "cule" : "csle";
  default: unreachable();
  }
}

static char *gen_binary(Node *node) {
  Type *lt = node->lhs->ty;
  Type *rt = node->rhs->ty;
  char *l = gen_expr(node->lhs);
  char *r = gen_expr(node->rhs);

  // pointer arithmetic (chibicc scaled the integer already)
  if (node->kind == ND_ADD && is_ptrish(lt)) {
    char *n = conv(r, rt, ty_long);
    char *t = tmp();
    emit("%s = offset ptr(u8) %s, i64 %s", t, l, n);
    return t;
  }
  if (node->kind == ND_SUB && is_ptrish(lt) && !is_ptrish(rt)) {
    char *n = conv(r, rt, ty_long);
    char *neg = tmp();
    emit("%s = sub i64 0, %s", neg, n);
    char *t = tmp();
    emit("%s = offset ptr(u8) %s, i64 %s", t, l, neg);
    return t;
  }
  if (is_ptrish(lt) || is_ptrish(rt)) {
    // pointer difference and comparisons: as addresses
    char *li = is_ptrish(lt) ? bitcast(l, "ptr(u8)", "u64") : conv(l, lt, ty_ulong);
    char *ri = is_ptrish(rt) ? bitcast(r, "ptr(u8)", "u64") : conv(r, rt, ty_ulong);
    char *t = tmp();
    if (node->kind == ND_SUB) {
      emit("%s = sub u64 %s, %s", t, li, ri);
      return bitcast(t, "u64", vty(node->ty));
    }
    emit("%s = %s u64 %s, %s", t, cmp_op(node->kind, ty_ulong), li, ri);
    return from_i1(t, node->ty);
  }

  char *ty = vty(lt);
  char *t = tmp();
  switch (node->kind) {
  case ND_ADD: emit("%s = add %s %s, %s", t, ty, l, r); return t;
  case ND_SUB: emit("%s = sub %s %s, %s", t, ty, l, r); return t;
  case ND_MUL: emit("%s = mul %s %s, %s", t, ty, l, r); return t;
  case ND_DIV:
    emit("%s = %s %s %s, %s", t, node->ty->is_unsigned ? "udiv" : "div", ty, l, r);
    return t;
  case ND_MOD:
    emit("%s = %s %s %s, %s", t, node->ty->is_unsigned ? "urem" : "rem", ty, l, r);
    return t;
  case ND_BITAND: emit("%s = and %s %s, %s", t, ty, l, r); return t;
  case ND_BITOR: emit("%s = or %s %s, %s", t, ty, l, r); return t;
  case ND_BITXOR: emit("%s = xor %s %s, %s", t, ty, l, r); return t;
  case ND_SHL:
    emit("%s = shl %s %s, %s", t, ty, l, int_resize(r, rt, lt));
    return t;
  case ND_SHR:
    emit("%s = %s %s %s, %s", t, lt->is_unsigned ? "shr" : "sar", ty, l, int_resize(r, rt, lt));
    return t;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
    emit("%s = %s %s %s, %s", t, cmp_op(node->kind, lt), ty, l, r);
    return from_i1(t, node->ty);
  default:
    error_tok(node->tok, "invalid expression");
  }
}

static char *gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    return NULL;
  case ND_NUM: {
    if (is_flonum(node->ty))
      return float_const(node->ty, node->fval);
    char *t = tmp();
    if (node->ty->is_unsigned)
      emit("%s = copy %s %lu", t, vty(node->ty), (unsigned long)node->val);
    else
      emit("%s = copy %s %ld", t, vty(node->ty), (long)node->val);
    return t;
  }
  case ND_NEG: {
    char *v = gen_expr(node->lhs);
    char *t = tmp();
    if (node->ty->kind == TY_DOUBLE) {
      char *b = bitcast(v, "f64", "u64");
      emit("%s = xor u64 %s, 9223372036854775808", t, b);
      return bitcast(t, "u64", "f64");
    }
    if (node->ty->kind == TY_FLOAT) {
      char *b = bitcast(v, "f32", "u32");
      emit("%s = xor u32 %s, 2147483648", t, b);
      return bitcast(t, "u32", "f32");
    }
    emit("%s = sub %s 0, %s", t, vty(node->ty), v);
    return t;
  }
  case ND_VAR:
    return load(node->ty, gen_addr(node));
  case ND_MEMBER: {
    char *a = gen_addr(node);
    if (node->member->is_bitfield)
      return bitfield_load(node->member, a);
    return load(node->ty, a);
  }
  case ND_DEREF:
    return load(node->ty, gen_expr(node->lhs));
  case ND_ADDR:
    return gen_addr(node->lhs);
  case ND_ASSIGN: {
    char *a = gen_addr(node->lhs);
    char *v = gen_expr(node->rhs);
    if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield) {
      bitfield_store(node->lhs->member, a, v, node->rhs->ty);
      return v;
    }
    store(node->ty, a, v);
    return is_agg(node->ty) ? a : v;
  }
  case ND_STMT_EXPR: {
    char *v = NULL;
    for (Node *n = node->body; n; n = n->next) {
      if (!n->next && n->kind == ND_EXPR_STMT)
        v = gen_expr(n->lhs);
      else
        gen_stmt(n);
    }
    return v;
  }
  case ND_COMMA:
    gen_expr(node->lhs);
    return gen_expr(node->rhs);
  case ND_CAST:
    return conv(gen_expr(node->lhs), node->lhs->ty, node->ty);
  case ND_MEMZERO:
    zero_mem(var_addr(node->var), node->var->ty->size, node->var->ty->align);
    return NULL;
  case ND_COND: {
    char *s = node->ty->kind == TY_VOID ? NULL : slot(is_agg(node->ty) ? "ptr(u8)" : vty(node->ty));
    char *lt = new_label(), *lf = new_label(), *le = new_label();
    br(cond_i1(node->cond), lt, lf);
    label(lt);
    char *a = gen_expr(node->then);
    if (s)
      store_as(s, is_agg(node->ty) ? "ptr(u8)" : vty(node->ty), conv(a, node->then->ty, node->ty));
    jmp(le);
    label(lf);
    char *b = gen_expr(node->els);
    if (s)
      store_as(s, is_agg(node->ty) ? "ptr(u8)" : vty(node->ty), conv(b, node->els->ty, node->ty));
    jmp(le);
    label(le);
    return s ? load_as(s, is_agg(node->ty) ? "ptr(u8)" : vty(node->ty)) : NULL;
  }
  case ND_NOT: {
    char *v = gen_expr(node->lhs);
    char *c = to_i1(v, node->lhs->ty);
    char *n = tmp();
    emit("%s = xor i1 %s, 1", n, c);
    return from_i1(n, node->ty);
  }
  case ND_BITNOT: {
    char *v = gen_expr(node->lhs);
    char *t = tmp();
    emit("%s = not %s %s", t, vty(node->ty), v);
    return t;
  }
  case ND_LOGAND:
  case ND_LOGOR: {
    bool is_and = node->kind == ND_LOGAND;
    char *s = slot("i32");
    store_as(s, "i32", is_and ? "0" : "1");
    char *lr = new_label(), *le = new_label();
    char *c1 = cond_i1(node->lhs);
    if (is_and)
      br(c1, lr, le);
    else
      br(c1, le, lr);
    label(lr);
    char *c2 = cond_i1(node->rhs);
    store_as(s, "i32", from_i1(c2, ty_int));
    jmp(le);
    label(le);
    return conv(load_as(s, "i32"), ty_int, node->ty);
  }
  case ND_FUNCALL:
    return gen_call(node);
  case ND_LABEL_VAL:
    unsupported(node->tok, "a label as a value");
  case ND_CAS:
  case ND_EXCH:
    unsupported(node->tok, "an atomic operation");
  case ND_VA_START:
  case ND_VA_END:
  case ND_VA_COPY:
  case ND_VA_ARG:
    unsupported(node->tok, "a variadic function definition");
  default:
    return gen_binary(node);
  }
}

// ── statements ──────────────────────────────────────────────────────────

static void gen_return_value(Node *e) {
  Type *rt = current_fn->ty->return_ty;
  char *v = gen_expr(e);
  if (rt->kind == TY_VOID) {
    emit("ret void");
  } else if (is_agg(rt)) {
    char *an = agg_type(rt);
    emit("ret %s %s", an, bitcast(v, "ptr(u8)", format("ptr(%s)", an)));
  } else {
    emit("ret %s %s", vty(rt), conv(v, e->ty, rt));
  }
  term = true;
}

static void gen_stmt(Node *node) {
  switch (node->kind) {
  case ND_IF: {
    char *lt = new_label(), *lf = new_label(), *le = new_label();
    br(cond_i1(node->cond), lt, lf);
    label(lt);
    gen_stmt(node->then);
    jmp(le);
    label(lf);
    if (node->els)
      gen_stmt(node->els);
    jmp(le);
    label(le);
    return;
  }
  case ND_FOR: {
    char *top = new_label(), *body = new_label();
    char *brk = lbl(node->brk_label), *cont = lbl(node->cont_label);
    if (node->init)
      gen_stmt(node->init);
    jmp(top);
    label(top);
    if (node->cond)
      br(cond_i1(node->cond), body, brk);
    else
      jmp(body);
    label(body);
    gen_stmt(node->then);
    jmp(cont);
    label(cont);
    if (node->inc)
      gen_expr(node->inc);
    jmp(top);
    label(brk);
    return;
  }
  case ND_DO: {
    char *top = new_label();
    char *brk = lbl(node->brk_label), *cont = lbl(node->cont_label);
    jmp(top);
    label(top);
    gen_stmt(node->then);
    jmp(cont);
    label(cont);
    br(cond_i1(node->cond), top, brk);
    label(brk);
    return;
  }
  case ND_SWITCH: {
    Type *ct = node->cond->ty;
    char *ty = vty(ct);
    char *v = gen_expr(node->cond);
    for (Node *n = node->case_next; n; n = n->case_next) {
      char *next = new_label();
      char *c = tmp();
      if (n->begin == n->end) {
        emit("%s = ceq %s %s, %ld", c, ty, v, n->begin);
      } else {
        // [GNU] case ranges: v - begin <= end - begin, unsigned
        char *d = tmp();
        emit("%s = sub %s %s, %ld", d, ty, v, n->begin);
        emit("%s = cule %s %s, %ld", c, ty, d, n->end - n->begin);
      }
      br(c, lbl(n->label), next);
      label(next);
    }
    if (node->default_case)
      jmp(lbl(node->default_case->label));
    else
      jmp(lbl(node->brk_label));
    gen_stmt(node->then);
    label(lbl(node->brk_label));
    return;
  }
  case ND_CASE:
    label(lbl(node->label));
    gen_stmt(node->lhs);
    return;
  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;
  case ND_GOTO:
    jmp(lbl(node->unique_label));
    return;
  case ND_GOTO_EXPR:
    unsupported(node->tok, "a computed goto");
  case ND_LABEL:
    label(lbl(node->unique_label));
    gen_stmt(node->lhs);
    return;
  case ND_RETURN:
    if (node->lhs) {
      gen_return_value(node->lhs);
    } else {
      emit("ret void");
      term = true;
    }
    return;
  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    return;
  case ND_ASM:
    unsupported(node->tok, "inline assembly");
  default:
    error_tok(node->tok, "invalid statement");
  }
}

// ── data ────────────────────────────────────────────────────────────────

static void emit_data(Obj *prog) {
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_function || !var->is_definition)
      continue;
    if (var->is_tls)
      unsupported(var->tok, "thread-local storage");
    fprintf(out, "data @%s = { ", gsym(var->name));
    if (!var->init_data) {
      fprintf(out, "skip %d }\n", var->ty->size > 0 ? var->ty->size : 1);
      continue;
    }
    Relocation *rel = var->rel;
    int pos = 0;
    bool first = true;
    while (pos < var->ty->size) {
      if (!first)
        fprintf(out, ", ");
      first = false;
      if (rel && rel->offset == pos) {
        if (rel->addend != 0)
          unsupported(var->tok, "an address constant with an offset");
        fprintf(out, "addr @%s", gsym(*rel->label));
        rel = rel->next;
        pos += 8;
      } else {
        fprintf(out, "b %d", (unsigned char)var->init_data[pos++]);
      }
    }
    if (first)
      fprintf(out, "skip 1");
    fprintf(out, " }\n");
  }
}

// ── functions ───────────────────────────────────────────────────────────

static void alloc_local(Obj *var) {
  if (var->ty->kind == TY_VLA)
    unsupported(var->tok, "a variable-length array");
  var->offset = ++nvar;
  int al = var->align >= 8 ? 8 : var->align >= 4 ? 4 : var->align >= 2 ? 2 : 1;
  char *unit = al == 8 ? "i64" : al == 4 ? "i32" : al == 2 ? "i16" : "u8";
  int n = (var->ty->size + al - 1) / al;
  if (n <= 0)
    n = 1;
  fprintf(ab, "    %%m%d = alloca %s, i64 %d\n", var->offset, unit, n);
  fprintf(ab, "    %%v%d = bitcast ptr(%s) %%m%d to ptr(u8)\n", var->offset, unit, var->offset);
}

static void emit_function(Obj *fn) {
  current_fn = fn;
  ntmp = 0;
  nlbl = 0;
  term = false;
  fb = open_memstream(&fb_buf, &fb_len);
  ab = open_memstream(&ab_buf, &ab_len);

  Type *rt = fn->ty->return_ty;
  Obj *params = fn->params;
  // chibicc's hidden buffer for a large struct result: sigil returns
  // aggregates itself
  if (is_agg(rt) && rt->size > 16 && params && !*params->name)
    params = params->next;
  if (fn->va_area)
    unsupported(fn->tok, "a variadic function definition");

  for (Obj *var = fn->locals; var; var = var->next)
    alloc_local(var);

  char *hbuf;
  size_t hlen;
  FILE *h = open_memstream(&hbuf, &hlen);
  fprintf(h, "%sfunc %s @%s(", fn->is_static ? "" : "pub ", rt->kind == TY_VOID ? "void" : aty(rt), gsym(fn->name));
  int k = 0;
  for (Obj *p = params; p; p = p->next, k++) {
    fprintf(h, "%s%s %%p%d", k ? ", " : "", aty(p->ty), k);
    // the incoming value into the parameter's slot
    if (is_agg(p->ty)) {
      char *an = agg_type(p->ty);
      char *src = bitcast(format("%%p%d", k), format("ptr(%s)", an), "ptr(u8)");
      copy_mem(format("%%v%d", p->offset), src, p->ty->size, p->ty->align);
    } else {
      store_as(format("%%v%d", p->offset), vty(p->ty), format("%%p%d", k));
    }
  }
  fprintf(h, ") {\n@entry:\n");
  fclose(h);

  gen_stmt(fn->body);
  if (!term) {
    if (rt->kind == TY_VOID)
      emit("ret void");
    else if (is_agg(rt))
      emit("halt");
    else if (is_flonum(rt))
      emit("ret %s %s", vty(rt), float_const(rt, 0));
    else
      emit("ret %s 0", vty(rt));
    term = true;
  }
  fclose(fb);
  fclose(ab);
  fprintf(out, "%s%s%s}\n\n", hbuf, ab_buf, fb_buf);
  free(hbuf);
  free(fb_buf);
  free(ab_buf);
}

void sigil_codegen(Obj *prog, FILE *o) {
  FILE *body;
  char *body_buf;
  size_t body_len;
  out = body = open_memstream(&body_buf, &body_len);
  tb = open_memstream(&tb_buf, &tb_len);

  for (Obj *v = prog; v; v = v->next)
    if (v->is_static)
      hashmap_put(&statics, v->name, (void *)1);

  emit_data(prog);
  for (Obj *fn = prog; fn; fn = fn->next) {
    if (!fn->is_function || !fn->is_definition || !fn->is_live)
      continue;
    emit_function(fn);
  }
  fclose(body);
  fclose(tb);
  fprintf(o, "// generated by sigcc (chibicc --backend=sigil, Sigma book 26.4.2)\n");
  fwrite(tb_buf, tb_len, 1, o);
  fwrite(body_buf, body_len, 1, o);
}
