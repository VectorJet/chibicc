// LLVM backend, selected with --backend=llvm.
//
// The backend lowers the AST produced by parse.c directly to LLVM IR through
// the LLVM-C API, verifies the module, and writes an object file with the LLVM
// target machine. The driver links objects with clang-19. Anything outside the
// supported subset is reported as a diagnostic. Nothing is silently dropped or
// approximated.
//
// Supported: integer and pointer scalars, fixed-size arrays, structs and unions
// as locals, parameters and globals (including string literals and global
// initializers that hold addresses of other globals or functions); direct
// calls; integer arithmetic, bitwise and comparison operators; pointer
// arithmetic, pointer difference and comparison; integer/pointer casts;
// short-circuit && and ||; ?:; casts between integer types and to/from bool;
// assignment and compound assignment (chibicc desugars the latter through
// ND_ADDR/ND_DEREF, which are handled here); if, for, while, do/while, switch
// (including GNU case ranges), goto and labels, break, continue and return;
// GNU statement expressions.
//
// Arrays, structs and unions are stored as byte arrays ([N x i8]); elements
// and members are accessed through byte-offset GEPs, which is what the
// frontend's pre-scaled pointer arithmetic and member offsets produce.
//
// Structs and unions are supported as objects, members (including bitfields
// and packed layouts), assignment, ?:, and by-value parameters, arguments and
// return values following the x86-64 SysV and AAPCS64 calling conventions.
//
// Not supported yet (diagnosed): floating point (including structs with
// floating-point members passed by value), VLAs, indirect calls, computed
// goto, thread-local variables, and anything else not listed above.

#include "chibicc.h"

#ifndef CHIBICC_LLVM

void llvm_codegen(Obj *prog, char *output_path, char *target) {
  (void)prog;
  (void)output_path;
  (void)target;
  error("this chibicc was built without LLVM support (rebuild with WITH_LLVM=1)");
}

#else

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

typedef struct Binding Binding;
struct Binding {
  Obj *obj;
  LLVMValueRef val;
  Binding *next;
};

typedef struct Label Label;
struct Label {
  char *name;
  LLVMBasicBlockRef bb;
  Label *next;
};

static LLVMContextRef ctx;
static LLVMModuleRef mod;
static LLVMBuilderRef bld;
static LLVMTargetMachineRef tm;

static Obj *program;          // all top-level objects, for resolving relocations
static Binding *global_binds; // global variables and functions
static Binding *local_binds;  // locals of the function being emitted
static Label *labels;         // blocks of the function being emitted, by unique label
static LLVMValueRef cur_fn;
static Type *cur_ret_ty;

static LLVMValueRef gen_expr(Node *node);
static LLVMValueRef gen_addr(Node *node);
static LLVMValueRef var_addr(Obj *var, Token *tok);
static LLVMValueRef get_global(Obj *var);
static LLVMValueRef get_function(Obj *fn, Token *tok);
static void gen_stmt(Node *node);

//
// Diagnostics
//

static noreturn void unsupported(Token *tok, char *what) {
  char *msg = format("LLVM backend: unsupported %s", what);
  if (tok)
    error_tok(tok, "%s", msg);
  error("%s", msg);
}

static char *tok_text(Token *tok) {
  if (!tok)
    return "?";
  return format("'%.*s'", tok->len, tok->loc);
}

static char *type_name(Type *ty) {
  if (ty->is_atomic)
    return "atomic";
  switch (ty->kind) {
  case TY_VOID:    return "void";
  case TY_FLOAT:   return "float";
  case TY_DOUBLE:  return "double";
  case TY_LDOUBLE: return "long double";
  case TY_ARRAY:
  case TY_VLA:     return "array";
  case TY_STRUCT:  return "struct";
  case TY_UNION:   return "union";
  case TY_FUNC:    return "function";
  case TY_PTR:     return "pointer";
  default:         return "value";
  }
}

//
// Types
//

static bool is_scalar(Type *ty) {
  // _Atomic objects need atomic loads, stores and memory ordering, which are not
  // implemented, so they are rejected wherever a scalar is required.
  return !ty->is_atomic && (is_integer(ty) || ty->kind == TY_PTR);
}

// Returns the LLVM type of a scalar C type, or NULL if it is not a scalar we
// support. Integers map to iN with N = 8 * sizeof(type); bool is i8 (0 or 1).
static LLVMTypeRef scalar_type(Type *ty) {
  if (is_integer(ty))
    return LLVMIntTypeInContext(ctx, ty->size * 8);
  if (ty->kind == TY_PTR)
    return LLVMPointerTypeInContext(ctx, 0);
  return NULL;
}

static bool is_array(Type *ty) {
  return ty->kind == TY_ARRAY && !ty->is_atomic && ty->size >= 0;
}

static bool is_aggregate(Type *ty) {
  return (ty->kind == TY_STRUCT || ty->kind == TY_UNION) && !ty->is_atomic && ty->size >= 0;
}

// Arrays, structs and unions live in memory. Their value in an expression is
// their address.
static bool is_memory(Type *ty) {
  return is_array(ty) || is_aggregate(ty);
}

// Types that can live in memory: scalars, fixed-size arrays, and complete
// structs and unions.
static bool is_storable(Type *ty) {
  return is_scalar(ty) || is_memory(ty);
}

static LLVMTypeRef i8_type(void) {
  return LLVMInt8TypeInContext(ctx);
}

static LLVMTypeRef i64_type(void) {
  return LLVMInt64TypeInContext(ctx);
}

static LLVMTypeRef ptr_type(void) {
  return LLVMPointerTypeInContext(ctx, 0);
}

// LLVM type of an object's storage. Arrays, structs and unions are opaque byte
// arrays; their members are accessed through byte-offset GEPs.
static LLVMTypeRef storage_type(Type *ty) {
  if (is_memory(ty))
    return LLVMArrayType2(i8_type(), ty->size);
  return scalar_type(ty);
}

//
// Calling convention
//
// LLVM leaves the C ABI of aggregates to the frontend: structs and unions must
// be lowered to the IR types the target's calling convention lowering expects,
// as clang does. Implemented for x86-64 SysV and AAPCS64, for aggregates
// without floating-point members (floating point is not supported yet).

typedef enum {
  ARCH_X86_64,
  ARCH_AARCH64,
} Arch;

static Arch arch;

typedef enum {
  PASS_DIRECT, // scalar, passed as its own LLVM type
  PASS_COERCE, // aggregate, passed as `ty` loaded from its memory image
  PASS_BYVAL,  // aggregate, pointer with the byval attribute (x86-64 memory class)
  PASS_BYREF,  // aggregate, pointer to a caller-made copy (AAPCS64 > 16 bytes)
} PassKind;

typedef struct {
  PassKind kind;
  LLVMTypeRef ty; // LLVM parameter (or return) type
  Type *cty;      // C type
} ArgABI;

typedef struct {
  bool sret;      // aggregate returned through a hidden first pointer parameter
  ArgABI ret;     // when !sret and the return type is not void
  int nargs;
  ArgABI *args;   // fixed parameters, then any variadic arguments of a call
  LLVMTypeRef fty;
} FuncABI;

static FuncABI *cur_abi;      // ABI of the function being emitted
static LLVMValueRef cur_sret; // its hidden return-buffer parameter, if any

static int max_int(int a, int b) {
  return a > b ? a : b;
}

static bool has_flonum(Type *ty) {
  if (is_flonum(ty))
    return true;
  if (ty->kind == TY_ARRAY)
    return has_flonum(ty->base);
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION)
    for (Member *m = ty->members; m; m = m->next)
      if (has_flonum(m->ty))
        return true;
  return false;
}

// x86-64 SysV classifies aggregates with misaligned members (from
// __attribute__((packed))) as MEMORY.
static bool has_unaligned_member(Type *ty) {
  if (ty->kind == TY_ARRAY)
    return has_unaligned_member(ty->base);
  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
    return false;
  for (Member *m = ty->members; m; m = m->next)
    if ((!m->is_bitfield && m->offset % m->ty->align) || has_unaligned_member(m->ty))
      return true;
  return false;
}

static void check_aggregate_abi(Type *ty, Token *tok) {
  if (has_flonum(ty))
    unsupported(tok, format("%s with floating-point members passed or returned by value",
                            type_name(ty)));
  if (ty->size == 0)
    unsupported(tok, format("empty %s passed or returned by value", type_name(ty)));
}

// AAPCS64 passes arguments according to their natural alignment: the largest
// member alignment, ignoring an aligned attribute on the aggregate type itself
// (GCC and clang agree on this).
static int natural_align(Type *ty) {
  if (ty->kind == TY_ARRAY)
    return natural_align(ty->base);
  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
    return ty->align;
  int align = 1;
  for (Member *m = ty->members; m; m = m->next)
    align = max_int(align, m->align);
  return align;
}

// True if any member of `ty` (at `base` within the outermost object) overlaps
// the byte range [lo, hi). x86-64 SysV does not pass an eightbyte that holds
// only padding (class NO_CLASS).
static bool has_data_in(Type *ty, int base, int lo, int hi) {
  if (ty->kind == TY_ARRAY) {
    for (int i = 0; i < ty->array_len; i++)
      if (has_data_in(ty->base, base + i * ty->base->size, lo, hi))
        return true;
    return false;
  }
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    for (Member *m = ty->members; m; m = m->next)
      if (has_data_in(m->ty, base + m->offset, lo, hi))
        return true;
    return false;
  }
  return base < hi && lo < base + ty->size;
}

// Integer type(s) holding the memory image of an aggregate of at most 16
// bytes in general-purpose registers. On AAPCS64 a 16-byte-aligned aggregate
// is passed as i128, which starts at an even-numbered register.
static LLVMTypeRef int_regs_type(Type *ty, int align) {
  if (arch == ARCH_AARCH64 && align >= 16)
    return LLVMInt128TypeInContext(ctx);
  if (ty->size <= 8 || (arch == ARCH_X86_64 && !has_data_in(ty, 0, 8, 16)))
    return i64_type();
  if (arch == ARCH_AARCH64)
    return LLVMArrayType2(i64_type(), 2);
  LLVMTypeRef pair[2] = {i64_type(), i64_type()};
  return LLVMStructTypeInContext(ctx, pair, 2, false);
}

// `gprs` counts the x86-64 integer argument registers left. An aggregate that
// needs more registers than are left goes entirely to memory, which the
// frontend has to decide because LLVM would split it between registers and the
// stack.
static ArgABI classify_arg(Type *ty, int *gprs, Token *tok) {
  ArgABI a = {PASS_DIRECT, NULL, ty};
  if (is_scalar(ty)) {
    a.ty = scalar_type(ty);
    if (*gprs > 0)
      (*gprs)--;
    return a;
  }
  if (!is_aggregate(ty))
    unsupported(tok, format("parameter type %s", type_name(ty)));
  check_aggregate_abi(ty, tok);

  if (arch == ARCH_AARCH64) {
    if (ty->size > 16) {
      a.kind = PASS_BYREF;
      a.ty = ptr_type();
      return a;
    }
    a.kind = PASS_COERCE;
    a.ty = int_regs_type(ty, natural_align(ty));
    return a;
  }

  int need = (ty->size > 8 && has_data_in(ty, 0, 8, 16)) ? 2 : 1;
  if (ty->size > 16 || has_unaligned_member(ty) || need > *gprs) {
    a.kind = PASS_BYVAL;
    a.ty = ptr_type();
    return a;
  }
  *gprs -= need;
  a.kind = PASS_COERCE;
  a.ty = int_regs_type(ty, ty->align);
  return a;
}

static FuncABI *func_abi(Type *fty, Node *extra_args, Token *tok) {
  FuncABI *abi = calloc(1, sizeof(FuncABI));
  int gprs = 6;
  Type *rt = fty->return_ty;
  LLVMTypeRef ret = LLVMVoidTypeInContext(ctx);

  if (rt->kind == TY_VOID) {
    // nothing
  } else if (is_scalar(rt)) {
    abi->ret = (ArgABI){PASS_DIRECT, scalar_type(rt), rt};
    ret = abi->ret.ty;
  } else if (is_aggregate(rt)) {
    check_aggregate_abi(rt, tok);
    bool memory = rt->size > 16 || (arch == ARCH_X86_64 && has_unaligned_member(rt));
    if (memory) {
      abi->sret = true;
      abi->ret.cty = rt;
      if (arch == ARCH_X86_64)
        gprs--; // the hidden pointer takes %rdi; AAPCS64 uses x8
    } else {
      abi->ret = (ArgABI){PASS_COERCE, int_regs_type(rt, rt->align), rt};
      ret = abi->ret.ty;
    }
  } else {
    unsupported(tok, format("return type %s", type_name(rt)));
  }

  int n = 0;
  for (Type *p = fty->params; p; p = p->next)
    n++;
  for (Node *a = extra_args; a; a = a->next)
    n++;

  abi->nargs = n;
  abi->args = calloc(n ? n : 1, sizeof(ArgABI));
  int i = 0;
  for (Type *p = fty->params; p; p = p->next)
    abi->args[i++] = classify_arg(p, &gprs, tok);
  for (Node *a = extra_args; a; a = a->next) {
    // Arrays among variadic arguments are not converted by the frontend; they
    // are passed as the decayed pointer.
    Type *ty = is_array(a->ty) ? pointer_to(a->ty->base) : a->ty;
    abi->args[i++] = classify_arg(ty, &gprs, a->tok);
  }

  LLVMTypeRef *params = calloc(n + 1, sizeof(LLVMTypeRef));
  int np = 0;
  if (abi->sret)
    params[np++] = ptr_type();
  int nfixed = n;
  for (Node *a = extra_args; a; a = a->next)
    nfixed--;
  for (i = 0; i < nfixed; i++)
    params[np++] = abi->args[i].ty;
  abi->fty = LLVMFunctionType(ret, params, np, fty->is_variadic);
  return abi;
}

// LLVM parameter index of C argument i.
static int abi_param_index(FuncABI *abi, int i) {
  return i + (abi->sret ? 1 : 0);
}

static LLVMAttributeRef enum_attr(char *name, uint64_t val) {
  return LLVMCreateEnumAttribute(ctx, LLVMGetEnumAttributeKindForName(name, strlen(name)), val);
}

static LLVMAttributeRef type_attr(char *name, LLVMTypeRef ty) {
  return LLVMCreateTypeAttribute(ctx, LLVMGetEnumAttributeKindForName(name, strlen(name)), ty);
}

// Attaches the ABI attributes to a function (call == NULL) or a call site.
// LLVM attribute index 0 is the return value and i + 1 is parameter i.
static void add_abi_attrs(FuncABI *abi, LLVMValueRef fn, LLVMValueRef call) {
#define ADD_ATTR(idx, attr) \
  (call ? LLVMAddCallSiteAttribute(call, idx, attr) : LLVMAddAttributeAtIndex(fn, idx, attr))

  if (abi->sret) {
    ADD_ATTR(1, type_attr("sret", storage_type(abi->ret.cty)));
    ADD_ATTR(1, enum_attr("noalias", 0));
  }

  // x86-64 callers extend arguments and return values narrower than int to
  // 32 bits, and clang-compiled code relies on it. AAPCS64 (non-Darwin) does
  // not require it.
  if (arch == ARCH_X86_64 && !abi->sret && abi->ret.cty &&
      is_integer(abi->ret.cty) && abi->ret.cty->size < 4)
    ADD_ATTR(0, enum_attr(abi->ret.cty->is_unsigned ? "zeroext" : "signext", 0));

  for (int i = 0; i < abi->nargs; i++) {
    ArgABI *a = &abi->args[i];
    int idx = abi_param_index(abi, i) + 1;
    if (a->kind == PASS_BYVAL) {
      ADD_ATTR(idx, type_attr("byval", storage_type(a->cty)));
      ADD_ATTR(idx, enum_attr("align", max_int(8, a->cty->align)));
    } else if (a->kind == PASS_DIRECT && arch == ARCH_X86_64 && is_integer(a->cty) &&
               a->cty->size < 4) {
      ADD_ATTR(idx, enum_attr(a->cty->is_unsigned ? "zeroext" : "signext", 0));
    }
  }
#undef ADD_ATTR
}

//
// Target selection
//
// Target selection is kept apart from AST lowering. This milestone enables only
// the host triple. The other known triples need their sysroot, ABI and
// frontend assumptions checked first (see the milestone report).

static const char *known_targets[] = {
  "x86_64-unknown-linux-gnu",
  "aarch64-unknown-linux-gnu",
  "riscv64-unknown-linux-gnu",
  "wasm32-wasi",
};

static char *resolve_target(char *requested) {
  char *msg = LLVMGetDefaultTargetTriple();
  char *host = strdup(msg);
  LLVMDisposeMessage(msg);

  if (requested && strcmp(requested, host)) {
    for (size_t i = 0; i < sizeof(known_targets) / sizeof(*known_targets); i++)
      if (!strcmp(requested, known_targets[i]))
        error("target '%s' is known but not enabled yet; this build targets only the host '%s'",
              requested, host);
    error("unknown target '%s' (host is '%s')", requested, host);
  }

  if (!strncmp(host, "x86_64-", 7))
    arch = ARCH_X86_64;
  else if (!strncmp(host, "aarch64-", 8))
    arch = ARCH_AARCH64;
  else
    error("LLVM backend: host '%s' has no calling convention implementation", host);
  return host;
}

//
// Symbol bindings
//

static LLVMValueRef find_binding(Binding *list, Obj *obj) {
  for (Binding *b = list; b; b = b->next)
    if (b->obj == obj)
      return b->val;
  return NULL;
}

static void add_binding(Binding **list, Obj *obj, LLVMValueRef val) {
  Binding *b = calloc(1, sizeof(Binding));
  b->obj = obj;
  b->val = val;
  b->next = *list;
  *list = b;
}

// Reads a little-endian integer from a global's initializer bytes.
static uint64_t read_le(char *p, int size) {
  uint64_t v = 0;
  for (int i = 0; i < size && i < 8; i++)
    v |= (uint64_t)(unsigned char)p[i] << (8 * i);
  return v;
}

static LLVMValueRef bytes_const(char *p, int len) {
  return LLVMConstStringInContext2(ctx, p, len, true);
}

// Initializer of a global without relocations.
static LLVMValueRef plain_initializer(Obj *var, LLVMTypeRef ty) {
  if (!var->init_data)
    return LLVMConstNull(ty);
  if (is_memory(var->ty))
    return bytes_const(var->init_data, var->ty->size);

  uint64_t v = read_le(var->init_data, var->ty->size);
  if (var->ty->kind == TY_PTR)
    return v ? LLVMConstIntToPtr(LLVMConstInt(i64_type(), v, false), ty) : LLVMConstNull(ty);
  return LLVMConstInt(ty, v, false);
}

// The frontend serializes every relocated word as 8 bytes.
#define RELOC_SIZE 8

// A global whose initializer holds addresses (e.g. `char *s = "abc";` or a
// table of pointers) is laid out as a packed struct of byte runs and pointers.
// Its type is computed first so the global can be bound before its
// initializer is built; the initializer may refer back to the global itself.
static LLVMTypeRef relocated_type(Obj *var) {
  int nrel = 0;
  for (Relocation *r = var->rel; r; r = r->next)
    nrel++;

  // Each relocation contributes at most a byte run and a pointer.
  LLVMTypeRef *elems = calloc(2 * nrel + 1, sizeof(LLVMTypeRef));
  int n = 0, pos = 0;
  for (Relocation *r = var->rel; r; r = r->next) {
    if (r->offset < pos)
      unsupported(var->tok, format("overlapping relocations in '%s'", var->name));
    if (r->offset > pos)
      elems[n++] = LLVMArrayType2(i8_type(), r->offset - pos);
    elems[n++] = LLVMPointerTypeInContext(ctx, 0);
    pos = r->offset + RELOC_SIZE;
  }
  if (pos < var->ty->size)
    elems[n++] = LLVMArrayType2(i8_type(), var->ty->size - pos);
  return LLVMStructTypeInContext(ctx, elems, n, true);
}

static Obj *find_toplevel(char *name) {
  for (Obj *o = program; o; o = o->next)
    if (!strcmp(o->name, name))
      return o;
  return NULL;
}

static LLVMValueRef relocated_initializer(Obj *var, LLVMTypeRef ty) {
  int n = LLVMCountStructElementTypes(ty);
  LLVMValueRef *vals = calloc(n, sizeof(LLVMValueRef));
  int i = 0, pos = 0;
  for (Relocation *r = var->rel; r; r = r->next) {
    if (r->offset > pos)
      vals[i++] = bytes_const(var->init_data + pos, r->offset - pos);

    Obj *target = find_toplevel(*r->label);
    if (!target)
      unsupported(var->tok, format("address of label in initializer of '%s'", var->name));
    LLVMValueRef base = target->is_function ? get_function(target, var->tok) : get_global(target);
    if (r->addend) {
      LLVMValueRef off = LLVMConstInt(i64_type(), r->addend, true);
      base = LLVMConstGEP2(i8_type(), base, &off, 1);
    }
    vals[i++] = base;
    pos = r->offset + RELOC_SIZE;
  }
  if (pos < var->ty->size)
    vals[i++] = bytes_const(var->init_data + pos, var->ty->size - pos);
  return LLVMConstStructInContext(ctx, vals, n, true);
}

// The frontend creates a separate Obj for every file-scope declaration of a
// variable: extern declarations, tentative definitions and the definition.
// scan_globals drops tentative definitions made redundant by a definition, but
// the AST can still refer to any of them. They all denote one object: the
// definition if there is one, else a surviving tentative definition.
static Obj *canonical_global(Obj *var) {
  Obj *best = var;
  for (Obj *o = program; o; o = o->next) {
    if (o->is_function || strcmp(o->name, var->name))
      continue;
    if (o->is_definition && !o->is_tentative)
      return o;
    if (o->is_tentative)
      best = o;
  }
  return best;
}

// Globals are created on first reference. chibicc also creates anonymous
// string-literal globals (including one for __func__ in every function), and
// eagerly lowering all of them would emit unused data.
static LLVMValueRef get_global(Obj *var) {
  LLVMValueRef g = find_binding(global_binds, var);
  if (g)
    return g;

  Obj *canon = canonical_global(var);
  if (canon != var) {
    g = get_global(canon);
    add_binding(&global_binds, var, g);
    return g;
  }

  if (var->is_tls)
    unsupported(var->tok, format("thread-local variable '%s'", var->name));
  if (!is_storable(var->ty))
    unsupported(var->tok, format("variable '%s' of type %s", var->name, type_name(var->ty)));

  bool has_storage = var->is_definition || var->is_tentative;
  LLVMTypeRef ty = (has_storage && var->rel) ? relocated_type(var) : storage_type(var->ty);
  g = LLVMAddGlobal(mod, ty, var->name);
  add_binding(&global_binds, var, g);

  if (var->is_static)
    LLVMSetLinkage(g, LLVMInternalLinkage);
  else if (var->is_tentative)
    LLVMSetLinkage(g, LLVMCommonLinkage);
  else
    LLVMSetLinkage(g, LLVMExternalLinkage);

  if (var->align > 0)
    LLVMSetAlignment(g, var->align);

  // extern declarations get no initializer, so no storage is emitted here.
  if (has_storage)
    LLVMSetInitializer(g, var->rel ? relocated_initializer(var, ty) : plain_initializer(var, ty));
  return g;
}

static LLVMValueRef get_function(Obj *fn, Token *tok) {
  LLVMValueRef f = find_binding(global_binds, fn);
  if (f)
    return f;

  if (!fn->is_definition && !strcmp(fn->name, "alloca"))
    unsupported(tok, "alloca");

  FuncABI *abi = func_abi(fn->ty, NULL, fn->tok ? fn->tok : tok);
  f = LLVMAddFunction(mod, fn->name, abi->fty);
  add_abi_attrs(abi, f, NULL);
  if (fn->is_static)
    LLVMSetLinkage(f, LLVMInternalLinkage);

  add_binding(&global_binds, fn, f);
  return f;
}

// Address of a variable. Functions are represented by their LLVM function.
static LLVMValueRef var_addr(Obj *var, Token *tok) {
  if (var->ty->kind == TY_FUNC)
    return get_function(var, tok);
  if (!is_storable(var->ty))
    unsupported(tok, format("variable '%s' of type %s", var->name, type_name(var->ty)));

  LLVMValueRef v = var->is_local ? find_binding(local_binds, var) : get_global(var);
  if (!v)
    unsupported(tok, format("storage for local '%s'", var->name));
  return v;
}

//
// Blocks and labels
//

static LLVMBasicBlockRef new_block(char *name) {
  return LLVMAppendBasicBlockInContext(ctx, cur_fn, name);
}

static void position(LLVMBasicBlockRef bb) {
  LLVMPositionBuilderAtEnd(bld, bb);
}

static bool is_terminated(void) {
  return LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(bld)) != NULL;
}

static void branch_if_open(LLVMBasicBlockRef bb) {
  if (!is_terminated())
    LLVMBuildBr(bld, bb);
}

// Code after return/break/continue is unreachable but still has to be valid
// IR, so it goes into a fresh block with no predecessors.
static void ensure_open(void) {
  if (is_terminated())
    position(new_block("unreachable"));
}

// Labels are the frontend's unique label names: break/continue targets of
// loops and switches, case labels, and user labels. A goto may precede its
// label, so label blocks are created on first reference.
static void add_label(char *name, LLVMBasicBlockRef bb) {
  if (!name)
    return;
  Label *l = calloc(1, sizeof(Label));
  l->name = name;
  l->bb = bb;
  l->next = labels;
  labels = l;
}

static LLVMBasicBlockRef label_block(char *name) {
  for (Label *l = labels; l; l = l->next)
    if (!strcmp(l->name, name))
      return l->bb;
  LLVMBasicBlockRef bb = new_block("label");
  add_label(name, bb);
  return bb;
}

// Ends the current block by falling through into a labeled block and continues
// emitting there.
static void enter_label(char *name) {
  LLVMBasicBlockRef bb = label_block(name);
  branch_if_open(bb);
  position(bb);
}

//
// Memory
//

// Temporaries go in the entry block so that a loop does not grow the stack.
static LLVMValueRef entry_alloca(LLVMTypeRef ty, int align) {
  LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(cur_fn);
  LLVMBuilderRef b = LLVMCreateBuilderInContext(ctx);
  LLVMValueRef first = LLVMGetFirstInstruction(entry);
  if (first)
    LLVMPositionBuilderBefore(b, first);
  else
    LLVMPositionBuilderAtEnd(b, entry);
  LLVMValueRef v = LLVMBuildAlloca(b, ty, "");
  LLVMSetAlignment(v, align);
  LLVMDisposeBuilder(b);
  return v;
}

static LLVMValueRef load(LLVMTypeRef ty, LLVMValueRef addr, int align) {
  LLVMValueRef v = LLVMBuildLoad2(bld, ty, addr, "");
  LLVMSetAlignment(v, align);
  return v;
}

static void store(LLVMValueRef v, LLVMValueRef addr, int align) {
  LLVMSetAlignment(LLVMBuildStore(bld, v, addr), align);
}

static void copy_mem(LLVMValueRef dst, int dst_align, LLVMValueRef src, int src_align, int size) {
  LLVMBuildMemCpy(bld, dst, dst_align, src, src_align, LLVMConstInt(i64_type(), size, false));
}

// Alignment known for the address of an lvalue or memory-typed expression.
// Members are at most as aligned as their offset within the enclosing object
// allows, which matters for packed structs.
static int known_align(Node *node) {
  if (node->kind != ND_MEMBER)
    return node->ty->align > 0 ? node->ty->align : 1;
  int align = known_align(node->lhs);
  int off = node->member->offset;
  while (off % align)
    align /= 2;
  return align;
}

// Spills a value of LLVM type `ty` holding an aggregate's memory image (an
// argument or return value in registers) and copies the aggregate out.
static void store_coerced(LLVMValueRef v, LLVMValueRef dst, Type *cty) {
  LLVMValueRef tmp = entry_alloca(LLVMTypeOf(v), 16);
  store(v, tmp, 16);
  copy_mem(dst, cty->align, tmp, 16, cty->size);
}

// The reverse of store_coerced: loads an aggregate's memory image as `ty`.
// The temporary is at least as large as `ty`, which may exceed the aggregate.
static LLVMValueRef load_coerced(LLVMTypeRef ty, LLVMValueRef src, Type *cty) {
  LLVMValueRef tmp = entry_alloca(ty, 16);
  store(LLVMConstNull(ty), tmp, 16);
  copy_mem(tmp, 16, src, cty->align, cty->size);
  return load(ty, tmp, 16);
}

//
// Value conversions
//

// Truth value (i1) of a scalar.
static LLVMValueRef to_bool(LLVMValueRef v) {
  if (LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind)
    return LLVMBuildIsNotNull(bld, v, "");
  return LLVMBuildICmp(bld, LLVMIntNE, v, LLVMConstNull(LLVMTypeOf(v)), "");
}

static LLVMValueRef int_coerce(LLVMValueRef v, LLVMTypeRef want, bool src_unsigned) {
  unsigned have_w = LLVMGetIntTypeWidth(LLVMTypeOf(v));
  unsigned want_w = LLVMGetIntTypeWidth(want);
  if (have_w == want_w)
    return v;
  if (have_w > want_w)
    return LLVMBuildTrunc(bld, v, want, "");
  return src_unsigned ? LLVMBuildZExt(bld, v, want, "") : LLVMBuildSExt(bld, v, want, "");
}

// Converts a scalar value of C type `from` to C type `to`.
static LLVMValueRef convert(LLVMValueRef v, Type *to, Type *from, Token *tok) {
  if (!v)
    unsupported(tok, "use of a void value");
  // An array value is already the address of its first element.
  if (is_array(from) && to->kind == TY_PTR)
    return v;
  // Struct and union values are addresses. The frontend only converts between
  // compatible types (assignment to a union, ?: and return).
  if (is_aggregate(from) && is_aggregate(to))
    return v;
  if (!is_scalar(to) || !is_scalar(from))
    unsupported(tok, format("conversion from %s to %s", type_name(from), type_name(to)));

  if (to->kind == TY_PTR) {
    if (from->kind == TY_PTR)
      return v;
    // Keep null pointer constants as `null` rather than `inttoptr 0`.
    if (LLVMIsAConstantInt(v) && LLVMConstIntGetZExtValue(v) == 0)
      return LLVMConstNull(scalar_type(to));
    return LLVMBuildIntToPtr(bld, int_coerce(v, i64_type(), from->is_unsigned),
                             scalar_type(to), "");
  }
  if (to->kind == TY_BOOL)
    return LLVMBuildZExt(bld, to_bool(v), scalar_type(to), "");
  if (from->kind == TY_PTR)
    return LLVMBuildPtrToInt(bld, v, scalar_type(to), "");
  return int_coerce(v, scalar_type(to), from->is_unsigned);
}

//
// Expressions
//

static LLVMValueRef gen_value(Node *node) {
  LLVMValueRef v = gen_expr(node);
  if (!v)
    unsupported(node->tok, "use of a void value");
  return v;
}

static LLVMValueRef gen_int(Node *node) {
  if (!is_integer(node->ty))
    unsupported(node->tok, format("non-integer operand of type %s", type_name(node->ty)));
  return gen_value(node);
}

static bool is_address(Type *ty) {
  return ty->kind == TY_PTR || is_array(ty);
}

// Byte offset operand of pointer arithmetic as an i64. The frontend has
// already scaled it by the element size, and for `p + n` the usual arithmetic
// conversions wrapped it in a cast to the pointer type, which is looked
// through here so the result can be a GEP instead of an inttoptr.
static LLVMValueRef byte_offset(Node *node) {
  if (node->kind == ND_CAST && node->ty->kind == TY_PTR && is_integer(node->lhs->ty))
    node = node->lhs;
  if (!is_integer(node->ty))
    unsupported(node->tok, format("pointer offset of type %s", type_name(node->ty)));
  return int_coerce(gen_value(node), i64_type(), node->ty->is_unsigned);
}

// ptr + n, ptr - n and ptr - ptr. The frontend divides a pointer difference by
// the element size with a separate ND_DIV, so ptr - ptr yields bytes here.
static LLVMValueRef gen_ptr_arith(Node *node) {
  if (node->kind == ND_SUB && is_integer(node->ty)) {
    if (!is_address(node->lhs->ty) || !is_address(node->rhs->ty))
      unsupported(node->tok, "pointer arithmetic");
    LLVMValueRef l = LLVMBuildPtrToInt(bld, gen_value(node->lhs), i64_type(), "");
    LLVMValueRef r = LLVMBuildPtrToInt(bld, gen_value(node->rhs), i64_type(), "");
    return int_coerce(LLVMBuildSub(bld, l, r, ""), scalar_type(node->ty), false);
  }

  if ((node->kind != ND_ADD && node->kind != ND_SUB) || !is_address(node->lhs->ty))
    unsupported(node->tok, "pointer arithmetic");

  LLVMValueRef base = gen_value(node->lhs);
  LLVMValueRef off = byte_offset(node->rhs);
  if (node->kind == ND_SUB)
    off = LLVMBuildNeg(bld, off, "");
  return LLVMBuildGEP2(bld, i8_type(), base, &off, 1, "");
}

static LLVMValueRef gen_arith(Node *node) {
  if (is_address(node->ty) || is_address(node->lhs->ty))
    return gen_ptr_arith(node);
  if (!is_integer(node->ty) || !is_integer(node->lhs->ty) || !is_integer(node->rhs->ty))
    unsupported(node->tok, format("arithmetic on %s", type_name(node->ty)));

  LLVMValueRef l = gen_value(node->lhs);
  LLVMValueRef r = int_coerce(gen_value(node->rhs), LLVMTypeOf(l), node->rhs->ty->is_unsigned);
  bool uns = node->ty->is_unsigned;
  LLVMValueRef v;

  switch (node->kind) {
  case ND_ADD:    v = LLVMBuildAdd(bld, l, r, ""); break;
  case ND_SUB:    v = LLVMBuildSub(bld, l, r, ""); break;
  case ND_MUL:    v = LLVMBuildMul(bld, l, r, ""); break;
  case ND_DIV:    v = uns ? LLVMBuildUDiv(bld, l, r, "") : LLVMBuildSDiv(bld, l, r, ""); break;
  case ND_MOD:    v = uns ? LLVMBuildURem(bld, l, r, "") : LLVMBuildSRem(bld, l, r, ""); break;
  case ND_BITAND: v = LLVMBuildAnd(bld, l, r, ""); break;
  case ND_BITOR:  v = LLVMBuildOr(bld, l, r, ""); break;
  case ND_BITXOR: v = LLVMBuildXor(bld, l, r, ""); break;
  case ND_SHL:    v = LLVMBuildShl(bld, l, r, ""); break;
  case ND_SHR:    v = uns ? LLVMBuildLShr(bld, l, r, "") : LLVMBuildAShr(bld, l, r, ""); break;
  default:
    unsupported(node->tok, "arithmetic operator");
  }
  return int_coerce(v, scalar_type(node->ty), uns);
}

static LLVMValueRef gen_compare(Node *node) {
  if (!is_scalar(node->lhs->ty) || !is_scalar(node->rhs->ty))
    unsupported(node->tok, "comparison of non-scalar values");

  bool ptrs = node->lhs->ty->kind == TY_PTR;
  if (ptrs != (node->rhs->ty->kind == TY_PTR))
    unsupported(node->tok, "comparison between pointer and integer");

  LLVMValueRef l = gen_value(node->lhs);
  LLVMValueRef r = gen_value(node->rhs);
  if (!ptrs)
    r = int_coerce(r, LLVMTypeOf(l), node->rhs->ty->is_unsigned);

  bool uns = ptrs || node->lhs->ty->is_unsigned;
  LLVMIntPredicate pred;
  switch (node->kind) {
  case ND_EQ: pred = LLVMIntEQ; break;
  case ND_NE: pred = LLVMIntNE; break;
  case ND_LT: pred = uns ? LLVMIntULT : LLVMIntSLT; break;
  default:    pred = uns ? LLVMIntULE : LLVMIntSLE; break;
  }
  return LLVMBuildZExt(bld, LLVMBuildICmp(bld, pred, l, r, ""), scalar_type(node->ty), "");
}

// && and || short-circuit: the right operand is evaluated in its own block and
// merged with a phi.
static LLVMValueRef gen_logical(Node *node) {
  bool is_and = node->kind == ND_LOGAND;
  LLVMTypeRef i1 = LLVMInt1TypeInContext(ctx);

  LLVMValueRef lc = to_bool(gen_value(node->lhs));
  LLVMBasicBlockRef from = LLVMGetInsertBlock(bld);
  LLVMBasicBlockRef rhs_bb = new_block("logic.rhs");
  LLVMBasicBlockRef end_bb = new_block("logic.end");

  if (is_and)
    LLVMBuildCondBr(bld, lc, rhs_bb, end_bb);
  else
    LLVMBuildCondBr(bld, lc, end_bb, rhs_bb);

  position(rhs_bb);
  LLVMValueRef rc = to_bool(gen_value(node->rhs));
  LLVMBasicBlockRef rhs_end = LLVMGetInsertBlock(bld);
  LLVMBuildBr(bld, end_bb);

  position(end_bb);
  LLVMValueRef phi = LLVMBuildPhi(bld, i1, "");
  LLVMValueRef vals[2] = {LLVMConstInt(i1, is_and ? 0 : 1, false), rc};
  LLVMBasicBlockRef bbs[2] = {from, rhs_end};
  LLVMAddIncoming(phi, vals, bbs, 2);
  return LLVMBuildZExt(bld, phi, scalar_type(node->ty), "");
}

static LLVMValueRef gen_cond_expr(Node *node) {
  bool is_void = node->ty->kind == TY_VOID;
  if (!is_void && !is_scalar(node->ty) && !is_aggregate(node->ty))
    unsupported(node->tok, format("conditional expression of type %s", type_name(node->ty)));

  LLVMValueRef c = to_bool(gen_value(node->cond));
  LLVMBasicBlockRef then_bb = new_block("cond.then");
  LLVMBasicBlockRef else_bb = new_block("cond.else");
  LLVMBasicBlockRef end_bb = new_block("cond.end");
  LLVMBuildCondBr(bld, c, then_bb, else_bb);

  position(then_bb);
  LLVMValueRef tv = gen_expr(node->then);
  if (!is_void)
    tv = convert(tv, node->ty, node->then->ty, node->tok);
  LLVMBasicBlockRef then_end = LLVMGetInsertBlock(bld);
  LLVMBuildBr(bld, end_bb);

  position(else_bb);
  LLVMValueRef ev = gen_expr(node->els);
  if (!is_void)
    ev = convert(ev, node->ty, node->els->ty, node->tok);
  LLVMBasicBlockRef else_end = LLVMGetInsertBlock(bld);
  LLVMBuildBr(bld, end_bb);

  position(end_bb);
  if (is_void)
    return NULL;
  // A struct or union operand's value is its address.
  LLVMTypeRef ty = is_aggregate(node->ty) ? ptr_type() : scalar_type(node->ty);
  LLVMValueRef phi = LLVMBuildPhi(bld, ty, "");
  LLVMValueRef vals[2] = {tv, ev};
  LLVMBasicBlockRef bbs[2] = {then_end, else_end};
  LLVMAddIncoming(phi, vals, bbs, 2);
  return phi;
}

static LLVMValueRef gen_call(Node *node) {
  if (node->lhs->kind != ND_VAR || node->lhs->var->ty->kind != TY_FUNC)
    unsupported(node->tok, "indirect call");

  LLVMValueRef fn = get_function(node->lhs->var, node->tok);

  // Arguments past the declared parameters are variadic (or unprototyped).
  Node *extra = node->args;
  for (Type *p = node->func_ty->params; p && extra; p = p->next)
    extra = extra->next;
  FuncABI *abi = func_abi(node->func_ty, extra, node->tok);

  LLVMValueRef ret_buf = node->ret_buffer ? var_addr(node->ret_buffer, node->tok) : NULL;
  LLVMValueRef *args = calloc(abi->nargs + 1, sizeof(LLVMValueRef));
  int n = 0;
  if (abi->sret)
    args[n++] = ret_buf;

  bool variadic = false;
  int i = 0;
  for (Node *a = node->args; a; a = a->next, i++) {
    ArgABI *ai = &abi->args[i];
    if (a == extra)
      variadic = true;

    LLVMValueRef v = gen_value(a);
    switch (ai->kind) {
    case PASS_DIRECT:
      // Default argument promotion: integers narrower than int are passed as
      // int. Declared parameters were already converted by the frontend.
      if (variadic && is_integer(a->ty) && a->ty->size < 4)
        v = int_coerce(v, scalar_type(ty_int), a->ty->is_unsigned);
      break;
    case PASS_COERCE:
      v = load_coerced(ai->ty, v, a->ty);
      break;
    case PASS_BYVAL:
      // The call lowering makes the copy.
      break;
    case PASS_BYREF: {
      LLVMValueRef copy = entry_alloca(storage_type(a->ty), a->ty->align);
      copy_mem(copy, a->ty->align, v, known_align(a), a->ty->size);
      v = copy;
      break;
    }
    }
    args[n++] = v;
  }

  LLVMValueRef r = LLVMBuildCall2(bld, abi->fty, fn, args, n, "");
  add_abi_attrs(abi, NULL, r);

  if (node->ty->kind == TY_VOID)
    return NULL;
  if (abi->sret)
    return ret_buf;
  if (abi->ret.kind == PASS_COERCE) {
    store_coerced(r, ret_buf, node->ty);
    return ret_buf;
  }
  return r;
}

static LLVMValueRef member_addr(Node *node) {
  LLVMValueRef base = gen_addr(node->lhs);
  if (!node->member->offset)
    return base;
  LLVMValueRef off = LLVMConstInt(i64_type(), node->member->offset, false);
  return LLVMBuildGEP2(bld, i8_type(), base, &off, 1, "");
}

static bool bitfield_unsigned(Member *mem) {
  return mem->ty->is_unsigned || mem->ty->kind == TY_BOOL;
}

// Sign- or zero-extends the low `width` bits of v to v's full width.
static LLVMValueRef extend_bits(LLVMValueRef v, int width, bool is_unsigned) {
  LLVMTypeRef ty = LLVMTypeOf(v);
  int bits = LLVMGetIntTypeWidth(ty);
  if (width == bits)
    return v;
  LLVMValueRef sh = LLVMConstInt(ty, bits - width, false);
  v = LLVMBuildShl(bld, v, sh, "");
  return is_unsigned ? LLVMBuildLShr(bld, v, sh, "") : LLVMBuildAShr(bld, v, sh, "");
}

// A bitfield is stored in a unit of its declared type at the member's offset.
static LLVMValueRef load_bitfield(Node *node) {
  Member *mem = node->member;
  LLVMTypeRef ty = scalar_type(mem->ty);
  LLVMValueRef v = load(ty, member_addr(node), known_align(node));
  v = LLVMBuildLShr(bld, v, LLVMConstInt(ty, mem->bit_offset, false), "");
  return extend_bits(v, mem->bit_width, bitfield_unsigned(mem));
}

// Read-modify-write of the bitfield's unit. Returns the value the bitfield
// holds afterwards, as C requires of an assignment expression.
static LLVMValueRef store_bitfield(Node *lhs, Node *rhs, Token *tok) {
  Member *mem = lhs->member;
  LLVMTypeRef ty = scalar_type(mem->ty);
  LLVMValueRef addr = member_addr(lhs);
  int align = known_align(lhs);
  LLVMValueRef v = convert(gen_value(rhs), mem->ty, rhs->ty, tok);

  uint64_t low = mem->bit_width >= 64 ? ~0ULL : (1ULL << mem->bit_width) - 1;
  uint64_t mask = low << mem->bit_offset;
  LLVMValueRef field = LLVMBuildAnd(bld, v, LLVMConstInt(ty, low, false), "");
  field = LLVMBuildShl(bld, field, LLVMConstInt(ty, mem->bit_offset, false), "");
  LLVMValueRef old = load(ty, addr, align);
  old = LLVMBuildAnd(bld, old, LLVMConstInt(ty, ~mask, false), "");
  store(LLVMBuildOr(bld, old, field, ""), addr, align);
  return extend_bits(v, mem->bit_width, bitfield_unsigned(mem));
}

// Address of an lvalue. chibicc lowers compound assignment and ++/-- to
// assignments through ND_ADDR/ND_DEREF, so both forms are needed here.
static LLVMValueRef gen_addr(Node *node) {
  switch (node->kind) {
  case ND_VAR:
    return var_addr(node->var, node->tok);
  case ND_DEREF:
    if (!is_address(node->lhs->ty))
      unsupported(node->tok, "dereference of a non-pointer");
    return gen_value(node->lhs);
  case ND_MEMBER:
    if (node->member->is_bitfield)
      unsupported(node->tok, "address of a bitfield");
    return member_addr(node);
  case ND_COMMA:
    // [GNU] `(a, b) = c`; compound literals are also lowered to this form.
    gen_expr(node->lhs);
    return gen_addr(node->rhs);
  default:
    // Struct and union rvalues (call results, assignments, ?:, comma and
    // statement expressions) evaluate to the address of their object.
    if (is_aggregate(node->ty))
      return gen_value(node);
    unsupported(node->tok, format("address of or assignment to %s", tok_text(node->tok)));
  }
}

static LLVMValueRef gen_expr(Node *node) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    return NULL;

  case ND_NUM:
    if (node->ty->kind == TY_PTR) {
      if (node->val != 0)
        unsupported(node->tok, "non-null pointer constant");
      return LLVMConstNull(scalar_type(node->ty));
    }
    if (!is_integer(node->ty))
      unsupported(node->tok, format("constant of type %s", type_name(node->ty)));
    return LLVMConstInt(scalar_type(node->ty), (unsigned long long)node->val, false);

  case ND_VAR:
    if (node->var->ty->kind == TY_FUNC)
      unsupported(node->tok, "function used as a value");
    // The value of an array, struct or union is its address.
    if (is_memory(node->ty))
      return var_addr(node->var, node->tok);
    if (!is_scalar(node->ty))
      unsupported(node->tok, format("variable '%s' of type %s", node->var->name, type_name(node->ty)));
    return load(scalar_type(node->ty), var_addr(node->var, node->tok), known_align(node));

  case ND_DEREF:
    if (!is_address(node->lhs->ty))
      unsupported(node->tok, "dereference of a non-pointer");
    // Dereferencing a pointer to an array (e.g. a row of a 2D array), struct
    // or union yields that object, whose value is its address.
    if (is_memory(node->ty))
      return gen_value(node->lhs);
    if (!is_scalar(node->ty))
      unsupported(node->tok, format("dereference of %s", type_name(node->ty)));
    return load(scalar_type(node->ty), gen_value(node->lhs), known_align(node));

  case ND_MEMBER:
    if (node->member->is_bitfield)
      return load_bitfield(node);
    if (is_memory(node->ty))
      return member_addr(node);
    if (!is_scalar(node->ty))
      unsupported(node->tok, format("member of type %s", type_name(node->ty)));
    return load(scalar_type(node->ty), member_addr(node), known_align(node));

  case ND_ADDR:
    return gen_addr(node->lhs);

  case ND_ASSIGN: {
    if (node->lhs->kind == ND_MEMBER && node->lhs->member->is_bitfield)
      return store_bitfield(node->lhs, node->rhs, node->tok);
    if (is_aggregate(node->lhs->ty)) {
      LLVMValueRef addr = gen_addr(node->lhs);
      LLVMValueRef src = gen_value(node->rhs);
      copy_mem(addr, known_align(node->lhs), src, known_align(node->rhs), node->lhs->ty->size);
      return addr;
    }
    if (!is_scalar(node->lhs->ty))
      unsupported(node->tok, format("assignment to %s", type_name(node->lhs->ty)));
    LLVMValueRef addr = gen_addr(node->lhs);
    LLVMValueRef v = convert(gen_value(node->rhs), node->lhs->ty, node->rhs->ty, node->tok);
    store(v, addr, known_align(node->lhs));
    return v;
  }

  case ND_MEMZERO: {
    // Zero-initialization emitted by declarations such as `int x = 3;`.
    Obj *var = node->var;
    if (!var)
      unsupported(node->tok, "zero-initialization without a variable");
    if (!is_storable(var->ty))
      unsupported(node->tok, format("variable '%s' of type %s", var->name, type_name(var->ty)));
    LLVMValueRef addr = var_addr(var, node->tok);
    if (is_memory(var->ty))
      LLVMBuildMemSet(bld, addr, LLVMConstNull(i8_type()),
                      LLVMConstInt(i64_type(), var->ty->size, false), var->align);
    else
      store(LLVMConstNull(scalar_type(var->ty)), addr, var->align);
    return NULL;
  }
  case ND_COMMA:
    gen_expr(node->lhs);
    return gen_expr(node->rhs);

  case ND_CAST:
    if (node->ty->kind == TY_VOID) {
      gen_expr(node->lhs);
      return NULL;
    }
    return convert(gen_value(node->lhs), node->ty, node->lhs->ty, node->tok);

  case ND_NEG:
    return LLVMBuildNeg(bld, gen_int(node->lhs), "");

  case ND_BITNOT:
    return LLVMBuildNot(bld, gen_int(node->lhs), "");

  case ND_NOT:
    return LLVMBuildZExt(bld, LLVMBuildNot(bld, to_bool(gen_value(node->lhs)), ""),
                         scalar_type(node->ty), "");

  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_SHL:
  case ND_SHR:
    return gen_arith(node);

  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
    return gen_compare(node);

  case ND_LOGAND:
  case ND_LOGOR:
    return gen_logical(node);

  case ND_COND:
    return gen_cond_expr(node);

  case ND_FUNCALL:
    return gen_call(node);

  case ND_STMT_EXPR:
    // [GNU] ({ ... }) has the value of its last statement, which the frontend
    // guarantees is an expression statement.
    for (Node *n = node->body; n; n = n->next) {
      if (!n->next && n->kind == ND_EXPR_STMT) {
        ensure_open();
        return gen_expr(n->lhs);
      }
      gen_stmt(n);
    }
    return NULL;

  default:
    unsupported(node->tok, format("expression %s", tok_text(node->tok)));
  }
}

//
// Statements
//

// Return without a value, or falling off the end of a function: returns zero
// (or nothing, for void and sret functions).
static void ret_default(void) {
  if (cur_ret_ty->kind == TY_VOID || cur_abi->sret)
    LLVMBuildRetVoid(bld);
  else
    LLVMBuildRet(bld, LLVMConstNull(cur_abi->ret.ty));
}

static void gen_return(Node *node) {
  if (cur_ret_ty->kind == TY_VOID) {
    if (node->lhs)
      gen_expr(node->lhs);
    LLVMBuildRetVoid(bld);
    return;
  }

  if (!node->lhs) {
    ret_default();
    return;
  }

  if (is_aggregate(cur_ret_ty)) {
    LLVMValueRef src = gen_value(node->lhs);
    if (cur_abi->sret) {
      copy_mem(cur_sret, cur_ret_ty->align, src, known_align(node->lhs), cur_ret_ty->size);
      LLVMBuildRetVoid(bld);
    } else {
      LLVMBuildRet(bld, load_coerced(cur_abi->ret.ty, src, cur_ret_ty));
    }
    return;
  }

  LLVMValueRef v = convert(gen_value(node->lhs), cur_ret_ty, node->lhs->ty, node->tok);
  LLVMBuildRet(bld, v);
}

static void gen_if(Node *node) {
  LLVMValueRef c = to_bool(gen_value(node->cond));
  LLVMBasicBlockRef then_bb = new_block("if.then");
  LLVMBasicBlockRef else_bb = node->els ? new_block("if.else") : NULL;
  LLVMBasicBlockRef end_bb = new_block("if.end");
  LLVMBuildCondBr(bld, c, then_bb, node->els ? else_bb : end_bb);

  position(then_bb);
  gen_stmt(node->then);
  branch_if_open(end_bb);

  if (node->els) {
    position(else_bb);
    gen_stmt(node->els);
    branch_if_open(end_bb);
  }
  position(end_bb);
}

// for and while. continue jumps to the increment block, which falls into the
// condition check.
static void gen_for(Node *node) {
  if (node->init)
    gen_stmt(node->init);

  LLVMBasicBlockRef cond_bb = new_block("for.cond");
  LLVMBasicBlockRef body_bb = new_block("for.body");
  LLVMBasicBlockRef inc_bb = new_block("for.inc");
  LLVMBasicBlockRef end_bb = new_block("for.end");

  branch_if_open(cond_bb);
  position(cond_bb);
  if (node->cond)
    LLVMBuildCondBr(bld, to_bool(gen_value(node->cond)), body_bb, end_bb);
  else
    LLVMBuildBr(bld, body_bb);

  position(body_bb);
  add_label(node->brk_label, end_bb);
  add_label(node->cont_label, inc_bb);
  gen_stmt(node->then);
  branch_if_open(inc_bb);

  position(inc_bb);
  if (node->inc)
    gen_expr(node->inc);
  LLVMBuildBr(bld, cond_bb);

  position(end_bb);
}

static void gen_do(Node *node) {
  LLVMBasicBlockRef body_bb = new_block("do.body");
  LLVMBasicBlockRef cond_bb = new_block("do.cond");
  LLVMBasicBlockRef end_bb = new_block("do.end");

  branch_if_open(body_bb);
  position(body_bb);
  add_label(node->brk_label, end_bb);
  add_label(node->cont_label, cond_bb);
  gen_stmt(node->then);
  branch_if_open(cond_bb);

  position(cond_bb);
  LLVMBuildCondBr(bld, to_bool(gen_value(node->cond)), body_bb, end_bb);
  position(end_bb);
}

static bool case_value_used(Node *sw, Node *upto, long val) {
  for (Node *n = sw->case_next; n != upto; n = n->case_next)
    if (n->begin == n->end && n->begin == val)
      return true;
  return false;
}

// Case ranges are tested with compare-and-branch before the switch
// instruction, which handles the single-value cases.
static void gen_switch(Node *node) {
  Type *ty = node->cond->ty;
  if (!is_integer(ty))
    unsupported(node->cond->tok, format("switch on %s", type_name(ty)));

  // The controlling expression undergoes integer promotion.
  LLVMTypeRef cty = scalar_type(ty->size < 4 ? ty_int : ty);
  LLVMValueRef c = int_coerce(gen_value(node->cond), cty, ty->is_unsigned);

  LLVMBasicBlockRef end_bb = new_block("sw.end");
  add_label(node->brk_label, end_bb);
  LLVMBasicBlockRef default_bb = node->default_case ? label_block(node->default_case->label) : end_bb;

  int nsingle = 0;
  for (Node *n = node->case_next; n; n = n->case_next) {
    if (n->begin == n->end) {
      nsingle++;
      continue;
    }
    LLVMValueRef diff = LLVMBuildSub(bld, c, LLVMConstInt(cty, n->begin, true), "");
    LLVMValueRef in_range =
      LLVMBuildICmp(bld, LLVMIntULE, diff, LLVMConstInt(cty, n->end - n->begin, false), "");
    LLVMBasicBlockRef next = new_block("sw.range");
    LLVMBuildCondBr(bld, in_range, label_block(n->label), next);
    position(next);
  }

  LLVMValueRef sw = LLVMBuildSwitch(bld, c, default_bb, nsingle);
  for (Node *n = node->case_next; n; n = n->case_next) {
    if (n->begin != n->end)
      continue;
    if (case_value_used(node, n, n->begin))
      error_tok(n->tok, "duplicate case value");
    LLVMAddCase(sw, LLVMConstInt(cty, n->begin, true), label_block(n->label));
  }

  gen_stmt(node->then);
  branch_if_open(end_bb);
  position(end_bb);
}

static void gen_stmt(Node *node) {
  ensure_open();

  switch (node->kind) {
  case ND_NULL_EXPR:
    return;
  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;
  case ND_EXPR_STMT:
    gen_expr(node->lhs);
    return;
  case ND_RETURN:
    gen_return(node);
    return;
  case ND_IF:
    gen_if(node);
    return;
  case ND_FOR:
    gen_for(node);
    return;
  case ND_DO:
    gen_do(node);
    return;
  case ND_SWITCH:
    gen_switch(node);
    return;
  case ND_CASE:
    enter_label(node->label);
    gen_stmt(node->lhs);
    return;
  case ND_LABEL:
    enter_label(node->unique_label);
    gen_stmt(node->lhs);
    return;
  case ND_GOTO:
    // Also used for break and continue, whose targets are loop labels.
    LLVMBuildBr(bld, label_block(node->unique_label));
    return;
  default:
    unsupported(node->tok, format("statement %s", tok_text(node->tok)));
  }
}

//
// Functions and module
//

static void emit_function(Obj *fn) {
  LLVMValueRef f = get_function(fn, fn->tok);
  cur_fn = f;
  cur_ret_ty = fn->ty->return_ty;
  cur_abi = func_abi(fn->ty, NULL, fn->tok);
  cur_sret = cur_abi->sret ? LLVMGetParam(f, 0) : NULL;
  local_binds = NULL;
  labels = NULL;

  LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, f, "entry");
  position(entry);

  // Every local gets a stack slot in the entry block. Only scalars, fixed-size
  // arrays, structs and unions are supported; other locals are diagnosed if
  // they are referenced.
  for (Obj *v = fn->locals; v; v = v->next) {
    if (!is_storable(v->ty))
      continue;
    LLVMValueRef slot = LLVMBuildAlloca(bld, storage_type(v->ty), "");
    if (v->align > 0)
      LLVMSetAlignment(slot, v->align);
    add_binding(&local_binds, v, slot);
  }

  // For a struct or union returned in memory (larger than 16 bytes), the
  // frontend puts a hidden pointer local for the legacy backend first in the
  // parameter list. The return buffer here is cur_sret instead.
  Obj *params = fn->params;
  Type *rt = fn->ty->return_ty;
  if ((rt->kind == TY_STRUCT || rt->kind == TY_UNION) && rt->size > 16)
    params = params->next;

  int i = 0;
  for (Obj *p = params; p; p = p->next, i++) {
    LLVMValueRef slot = find_binding(local_binds, p);
    if (!slot)
      unsupported(p->tok, format("parameter '%s' of type %s", p->name, type_name(p->ty)));

    LLVMValueRef v = LLVMGetParam(f, abi_param_index(cur_abi, i));
    switch (cur_abi->args[i].kind) {
    case PASS_DIRECT:
      store(v, slot, p->align);
      break;
    case PASS_COERCE:
      store_coerced(v, slot, p->ty);
      break;
    case PASS_BYVAL:
    case PASS_BYREF:
      // The pointer is to a copy owned by this call; copying it into the
      // parameter's own slot keeps every local in one place.
      copy_mem(slot, p->align, v, p->ty->align, p->ty->size);
      break;
    }
  }

  gen_stmt(fn->body);

  if (!is_terminated())
    ret_default();
}

void llvm_codegen(Obj *prog, char *output_path, char *target) {
  if (!output_path || !strcmp(output_path, "-"))
    error("--backend=llvm needs an output file (use -o)");

  char *triple = resolve_target(target);

  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();

  char *msg = NULL;
  LLVMTargetRef t;
  if (LLVMGetTargetFromTriple(triple, &t, &msg))
    error("LLVM: cannot find target '%s': %s", triple, msg);

  tm = LLVMCreateTargetMachine(t, triple, "generic", "", LLVMCodeGenLevelDefault,
                               LLVMRelocPIC, LLVMCodeModelDefault);
  ctx = LLVMContextCreate();
  mod = LLVMModuleCreateWithNameInContext("chibicc", ctx);
  bld = LLVMCreateBuilderInContext(ctx);

  LLVMSetTarget(mod, triple);
  LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
  char *dl = LLVMCopyStringRepOfTargetData(td);
  LLVMSetDataLayout(mod, dl);
  LLVMDisposeMessage(dl);
  LLVMDisposeTargetData(td);

  program = prog;

  // Externally visible definitions are always emitted, or rejected if their
  // type is unsupported. Static objects (string literals, __func__, static
  // locals, file-scope statics) are created on first reference, so unused ones
  // are not emitted and are not diagnosed.
  for (Obj *v = prog; v; v = v->next)
    if (!v->is_function && !v->is_static && (v->is_definition || v->is_tentative))
      get_global(v);

  for (Obj *fn = prog; fn; fn = fn->next)
    if (fn->is_function && fn->is_definition && fn->is_live && fn->body)
      emit_function(fn);

  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &msg))
    error("internal error: LLVM module verification failed:\n%s", msg);

  if (LLVMTargetMachineEmitToFile(tm, mod, output_path, LLVMObjectFile, &msg))
    error("LLVM: cannot write object file '%s': %s", output_path, msg);
}

#endif
