// LLVM backend, selected with --backend=llvm.
//
// The backend lowers the AST produced by parse.c directly to LLVM IR through
// the LLVM-C API, verifies the module, and writes an object file with the LLVM
// target machine. The driver links objects with clang-19. Anything outside the
// supported subset is reported as a diagnostic. Nothing is silently dropped or
// approximated.
//
// Supported: integer (up to __int128), floating-point (float, double and long
// double) and pointer scalars, fixed-size arrays, structs and unions as
// locals, parameters and globals (including string literals and global
// initializers that hold addresses of other globals or functions); direct
// and indirect calls; variadic function definitions; integer and
// floating-point arithmetic, bitwise and comparison operators; pointer
// arithmetic, pointer difference and comparison; conversions between all
// scalar types; short-circuit && and ||; ?:; assignment and compound
// assignment (chibicc desugars the latter through ND_ADDR/ND_DEREF, which are
// handled here); if, for, while, do/while, switch (including GNU case ranges),
// goto and labels, break, continue and return; GNU statement expressions.
//
// Arrays, structs and unions are stored as byte arrays ([N x i8]); elements
// and members are accessed through byte-offset GEPs, which is what the
// frontend's pre-scaled pointer arithmetic and member offsets produce.
//
// Structs and unions are supported as objects, members (including bitfields
// and packed layouts), assignment, ?:, and by-value parameters, arguments and
// return values following the x86-64 SysV and AAPCS64 calling conventions.
//
// Not supported yet (diagnosed): VLAs, alloca, atomics, computed goto,
// thread-local variables, inline assembly, and anything else not listed above.

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
static uint64_t read_le(char *p, int size);

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
  return !ty->is_atomic && (is_integer(ty) || is_flonum(ty) || ty->kind == TY_PTR);
}

typedef enum {
  ARCH_X86_64,
  ARCH_AARCH64,
} Arch;

static Arch arch;

// long double is IEEE binary128 on AAPCS64 and the x87 80-bit format on
// x86-64 (stored in 16 bytes).
static LLVMTypeRef ldouble_type(void) {
  return arch == ARCH_AARCH64 ? LLVMFP128TypeInContext(ctx) : LLVMX86FP80TypeInContext(ctx);
}

// Returns the LLVM type of a scalar C type, or NULL if it is not a scalar we
// support. Integers map to iN with N = 8 * sizeof(type); bool is i8 (0 or 1).
static LLVMTypeRef scalar_type(Type *ty) {
  if (is_integer(ty))
    return LLVMIntTypeInContext(ctx, ty->size * 8);
  if (ty->kind == TY_FLOAT)
    return LLVMFloatTypeInContext(ctx);
  if (ty->kind == TY_DOUBLE)
    return LLVMDoubleTypeInContext(ctx);
  if (ty->kind == TY_LDOUBLE)
    return ldouble_type();
  if (ty->kind == TY_PTR)
    return LLVMPointerTypeInContext(ctx, 0);
  return NULL;
}

// Number of value bits of a floating-point type's memory image.
static int fp_bits(Type *ty) {
  if (ty->kind == TY_LDOUBLE)
    return arch == ARCH_AARCH64 ? 128 : 80;
  return ty->size * 8;
}

// A long double constant. The frontend computes long double values in the
// format of the machine chibicc runs on; printing the value as a hex float and
// letting LLVM parse it yields the target's format whatever that host is.
static LLVMValueRef ldouble_const(long double v) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%La", v);
  return LLVMConstRealOfStringAndSize(ldouble_type(), buf, n);
}

// A floating-point constant from its memory image (little-endian bytes).
static LLVMValueRef fp_const_from_bytes(Type *ty, char *p) {
  if (ty->kind == TY_LDOUBLE) {
    long double v;
    memcpy(&v, p, sizeof(v));
    return ldouble_const(v);
  }

  // float and double: reinterpret the IEEE bits.
  LLVMTypeRef ity = LLVMIntTypeInContext(ctx, ty->size * 8);
  return LLVMConstBitCast(LLVMConstInt(ity, read_le(p, ty->size), false), scalar_type(ty));
}

// Arrays of unknown size (`extern int a[];`) are included; they only ever
// denote storage defined elsewhere.
static bool is_array(Type *ty) {
  return ty->kind == TY_ARRAY && !ty->is_atomic;
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
    return LLVMArrayType2(i8_type(), ty->size > 0 ? ty->size : 0);
  return scalar_type(ty);
}

//
// Calling convention
//
// LLVM leaves the C ABI of aggregates to the frontend: structs and unions must
// be lowered to the IR types the target's calling convention lowering expects,
// as clang does. Implemented for x86-64 SysV and AAPCS64.

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
  if (ty->size == 0)
    unsupported(tok, format("empty %s passed or returned by value", type_name(ty)));
}

// AAPCS64 homogeneous floating-point aggregate: all leaf members have the same
// floating-point type and there are 1 to 4 of them. Returns that type and
// sets *count, or returns NULL.
static Type *hfa_leaf(Type *ty, Type *base) {
  if (is_flonum(ty))
    return (!base || base->kind == ty->kind) ? ty : NULL;
  if (ty->kind == TY_ARRAY)
    return ty->array_len > 0 ? hfa_leaf(ty->base, base) : NULL;
  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
    return NULL;
  for (Member *m = ty->members; m; m = m->next) {
    if (m->is_bitfield)
      return NULL;
    base = hfa_leaf(m->ty, base);
    if (!base)
      return NULL;
  }
  return base;
}

static Type *hfa_base(Type *ty, int *count) {
  if (!is_aggregate(ty) || ty->size == 0)
    return NULL;
  Type *base = hfa_leaf(ty, NULL);
  if (!base || ty->size % base->size)
    return NULL;
  *count = ty->size / base->size;
  return *count <= 4 ? base : NULL;
}

// x86-64 SysV classes of an eightbyte.
typedef enum {
  CLS_NONE,
  CLS_INT,
  CLS_SSE,
  CLS_MEM,
} X86Class;

static X86Class merge_class(X86Class a, X86Class b) {
  if (a == b || b == CLS_NONE)
    return a;
  if (a == CLS_NONE)
    return b;
  if (a == CLS_MEM || b == CLS_MEM)
    return CLS_MEM;
  return CLS_INT; // INTEGER and SSE merge to INTEGER
}

// Class of the leaf members of `ty` (at `base`) overlapping [lo, hi).
static X86Class classify_x86(Type *ty, int base, int lo, int hi) {
  if (ty->kind == TY_ARRAY) {
    X86Class c = CLS_NONE;
    for (int i = 0; i < ty->array_len; i++)
      c = merge_class(c, classify_x86(ty->base, base + i * ty->base->size, lo, hi));
    return c;
  }
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    X86Class c = CLS_NONE;
    for (Member *m = ty->members; m; m = m->next)
      c = merge_class(c, classify_x86(m->ty, base + m->offset, lo, hi));
    return c;
  }
  if (!(base < hi && lo < base + ty->size))
    return CLS_NONE;
  if (ty->kind == TY_LDOUBLE)
    return CLS_MEM; // X87 class: passed in memory
  return is_flonum(ty) ? CLS_SSE : CLS_INT;
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

// AAPCS64: integer type(s) holding the memory image of an aggregate of at
// most 16 bytes in general-purpose registers. A 16-byte-aligned aggregate is
// passed as i128, which starts at an even-numbered register.
static LLVMTypeRef aarch64_int_regs_type(Type *ty, int align) {
  if (align >= 16)
    return LLVMInt128TypeInContext(ctx);
  if (ty->size <= 8)
    return i64_type();
  return LLVMArrayType2(i64_type(), 2);
}

// AAPCS64: the coerced type of an HFA, [n x T], passed in n vector registers.
static LLVMTypeRef hfa_type(Type *base, int count) {
  return LLVMArrayType2(scalar_type(base), count);
}

// x86-64: the classes of the (at most two) eightbytes of an aggregate of at
// most 16 bytes. Returns false if it is passed in memory.
static bool x86_eightbyte_classes(Type *ty, X86Class cls[2]) {
  if (ty->size > 16 || has_unaligned_member(ty))
    return false;
  cls[0] = classify_x86(ty, 0, 0, 8);
  cls[1] = ty->size > 8 ? classify_x86(ty, 0, 8, 16) : CLS_NONE;
  if (cls[0] == CLS_MEM || cls[1] == CLS_MEM)
    return false;
  if (cls[0] == CLS_NONE)
    cls[0] = CLS_INT; // leading padding; pass the eightbyte anyway
  return true;
}

// x86-64: the coerced type of an aggregate passed in registers. Each eightbyte
// is an i64 (INTEGER) or a double (SSE); a double carries any SSE eightbyte,
// including one holding two floats, bit for bit.
static LLVMTypeRef x86_coerced_type(X86Class cls[2]) {
  LLVMTypeRef t[2];
  for (int i = 0; i < 2; i++)
    t[i] = cls[i] == CLS_SSE ? LLVMDoubleTypeInContext(ctx) : i64_type();
  if (cls[1] == CLS_NONE)
    return t[0];
  return LLVMStructTypeInContext(ctx, t, 2, false);
}

// x86-64 argument registers left: 6 general-purpose and 8 SSE.
typedef struct {
  int gprs;
  int fprs;
} RegsLeft;

// Parameter types are copies made when the function type was parsed. If a
// struct or union was still incomplete then, the copy stays incomplete while
// the original (`origin`) is completed in place by the later definition.
static Type *completed(Type *ty) {
  while (ty->size < 0 && ty->origin)
    ty = ty->origin;
  return ty;
}

// An x86-64 aggregate that needs more registers of either kind than are left
// goes entirely to memory, which the frontend has to decide because LLVM would
// split it between registers and the stack.
static ArgABI classify_arg(Type *ty, RegsLeft *regs, Token *tok) {
  ty = completed(ty);
  ArgABI a = {PASS_DIRECT, NULL, ty};
  if (is_scalar(ty)) {
    a.ty = scalar_type(ty);
    if (is_flonum(ty)) {
      if (ty->kind != TY_LDOUBLE && regs->fprs > 0)
        regs->fprs--;
    } else if (ty->size > 8) {
      // __int128 takes a register pair; LLVM spills it whole when one register
      // is left.
      if (regs->gprs >= 2)
        regs->gprs -= 2;
    } else if (regs->gprs > 0) {
      regs->gprs--;
    }
    return a;
  }
  if (!is_aggregate(ty))
    unsupported(tok, format("parameter type %s", type_name(ty)));
  check_aggregate_abi(ty, tok);

  if (arch == ARCH_AARCH64) {
    int n;
    Type *base = hfa_base(ty, &n);
    if (base) {
      a.kind = PASS_COERCE;
      a.ty = hfa_type(base, n);
      return a;
    }
    if (ty->size > 16) {
      a.kind = PASS_BYREF;
      a.ty = ptr_type();
      return a;
    }
    a.kind = PASS_COERCE;
    a.ty = aarch64_int_regs_type(ty, natural_align(ty));
    return a;
  }

  X86Class cls[2];
  if (x86_eightbyte_classes(ty, cls)) {
    int need_gp = (cls[0] == CLS_INT) + (cls[1] == CLS_INT);
    int need_fp = (cls[0] == CLS_SSE) + (cls[1] == CLS_SSE);
    if (need_gp <= regs->gprs && need_fp <= regs->fprs) {
      regs->gprs -= need_gp;
      regs->fprs -= need_fp;
      a.kind = PASS_COERCE;
      a.ty = x86_coerced_type(cls);
      return a;
    }
  }
  a.kind = PASS_BYVAL;
  a.ty = ptr_type();
  return a;
}

// How an aggregate is returned: NULL for memory (sret), else the coerced type.
static LLVMTypeRef aggregate_return_type(Type *ty) {
  if (arch == ARCH_AARCH64) {
    int n;
    Type *base = hfa_base(ty, &n);
    if (base)
      return hfa_type(base, n);
    if (ty->size > 16)
      return NULL;
    return aarch64_int_regs_type(ty, ty->align);
  }
  // An aggregate holding just a long double has classes X87, X87UP: returned
  // in %st0 (as an argument it goes to memory).
  int n;
  Type *base = hfa_base(ty, &n);
  if (base && base->kind == TY_LDOUBLE && n == 1)
    return ldouble_type();

  X86Class cls[2];
  if (!x86_eightbyte_classes(ty, cls))
    return NULL;
  return x86_coerced_type(cls);
}

static FuncABI *func_abi(Type *fty, Node *extra_args, Token *tok) {
  FuncABI *abi = calloc(1, sizeof(FuncABI));
  RegsLeft regs = {6, 8};
  Type *rt = completed(fty->return_ty);
  LLVMTypeRef ret = LLVMVoidTypeInContext(ctx);

  if (rt->kind == TY_VOID) {
    // nothing
  } else if (is_scalar(rt)) {
    abi->ret = (ArgABI){PASS_DIRECT, scalar_type(rt), rt};
    ret = abi->ret.ty;
  } else if (is_aggregate(rt)) {
    check_aggregate_abi(rt, tok);
    LLVMTypeRef coerced = aggregate_return_type(rt);
    if (!coerced) {
      abi->sret = true;
      abi->ret.cty = rt;
      if (arch == ARCH_X86_64)
        regs.gprs--; // the hidden pointer takes %rdi; AAPCS64 uses x8
    } else {
      abi->ret = (ArgABI){PASS_COERCE, coerced, rt};
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
    abi->args[i++] = classify_arg(p, &regs, tok);
  for (Node *a = extra_args; a; a = a->next) {
    // Arrays and functions among variadic arguments are not converted by the
    // frontend; they are passed as the decayed pointer.
    Type *ty = a->ty;
    if (is_array(ty))
      ty = pointer_to(ty->base);
    else if (ty->kind == TY_FUNC)
      ty = pointer_to(ty);
    abi->args[i++] = classify_arg(ty, &regs, a->tok);
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
    int hfa_n;
    Type *hfa;
    if (a->kind == PASS_BYVAL) {
      ADD_ATTR(idx, type_attr("byval", storage_type(a->cty)));
      ADD_ATTR(idx, enum_attr("align", max_int(8, a->cty->align)));
    } else if (arch == ARCH_AARCH64 && a->kind == PASS_COERCE &&
               (hfa = hfa_base(a->cty, &hfa_n))) {
      // An HFA that no longer fits in vector registers goes to the stack with
      // at least 8-byte alignment (as clang specifies), not its element's.
      ADD_ATTR(idx, enum_attr("alignstack", max_int(8, hfa->align)));
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

  if (is_flonum(var->ty))
    return fp_const_from_bytes(var->ty, var->init_data);

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
  LLVMTypeRef ty = LLVMTypeOf(v);
  if (LLVMGetTypeKind(ty) == LLVMPointerTypeKind)
    return LLVMBuildIsNotNull(bld, v, "");
  if (LLVMGetTypeKind(ty) != LLVMIntegerTypeKind)
    // Floating point: true unless equal to zero (NaN is true).
    return LLVMBuildFCmp(bld, LLVMRealUNE, v, LLVMConstNull(ty), "");
  return LLVMBuildICmp(bld, LLVMIntNE, v, LLVMConstNull(ty), "");
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
  // An array value is already the address of its first element, and a
  // function designator's value is the function's address.
  if ((is_array(from) || from->kind == TY_FUNC) && to->kind == TY_PTR)
    return v;
  // Struct and union values are addresses. The frontend only converts between
  // compatible types (assignment to a union, ?: and return).
  if (is_aggregate(from) && is_aggregate(to))
    return v;
  if (!is_scalar(to) || !is_scalar(from))
    unsupported(tok, format("conversion from %s to %s", type_name(from), type_name(to)));

  if (is_flonum(from) || is_flonum(to)) {
    if (from->kind == TY_PTR || to->kind == TY_PTR)
      unsupported(tok, "conversion between pointer and floating point");
    if (to->kind == TY_BOOL)
      return LLVMBuildZExt(bld, to_bool(v), scalar_type(to), "");
    if (is_flonum(from) && is_flonum(to)) {
      if (fp_bits(from) == fp_bits(to))
        return v;
      return fp_bits(from) < fp_bits(to) ? LLVMBuildFPExt(bld, v, scalar_type(to), "")
                                         : LLVMBuildFPTrunc(bld, v, scalar_type(to), "");
    }
    if (is_flonum(to))
      return from->is_unsigned ? LLVMBuildUIToFP(bld, v, scalar_type(to), "")
                               : LLVMBuildSIToFP(bld, v, scalar_type(to), "");
    return to->is_unsigned ? LLVMBuildFPToUI(bld, v, scalar_type(to), "")
                           : LLVMBuildFPToSI(bld, v, scalar_type(to), "");
  }

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

// The usual arithmetic conversions give both operands the result type.
static LLVMValueRef gen_fp_arith(Node *node) {
  LLVMValueRef l = gen_value(node->lhs);
  LLVMValueRef r = gen_value(node->rhs);
  switch (node->kind) {
  case ND_ADD: return LLVMBuildFAdd(bld, l, r, "");
  case ND_SUB: return LLVMBuildFSub(bld, l, r, "");
  case ND_MUL: return LLVMBuildFMul(bld, l, r, "");
  case ND_DIV: return LLVMBuildFDiv(bld, l, r, "");
  default:
    unsupported(node->tok, "floating-point operator");
  }
}

static LLVMValueRef gen_arith(Node *node) {
  if (is_address(node->ty) || is_address(node->lhs->ty))
    return gen_ptr_arith(node);
  if (is_flonum(node->ty) && is_flonum(node->lhs->ty) && is_flonum(node->rhs->ty))
    return gen_fp_arith(node);
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

  if (is_flonum(node->lhs->ty)) {
    // Ordered comparisons are false for NaN; != is true.
    LLVMRealPredicate fpred;
    switch (node->kind) {
    case ND_EQ: fpred = LLVMRealOEQ; break;
    case ND_NE: fpred = LLVMRealUNE; break;
    case ND_LT: fpred = LLVMRealOLT; break;
    default:    fpred = LLVMRealOLE; break;
    }
    return LLVMBuildZExt(bld, LLVMBuildFCmp(bld, fpred, l, r, ""), scalar_type(node->ty), "");
  }

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
  // A direct call names the function; anything else is a call through a
  // function pointer, whose value is the callee's address.
  LLVMValueRef fn;
  if (node->lhs->kind == ND_VAR && node->lhs->var->ty->kind == TY_FUNC)
    fn = get_function(node->lhs->var, node->tok);
  else
    fn = gen_value(node->lhs);

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

//
// Variadic arguments
//
// va_start, va_end and va_copy are LLVM intrinsics. va_arg is expanded here
// following each ABI's algorithm (as clang does): LLVM's own va_arg
// instruction does not implement the AAPCS64 va_list.

static LLVMValueRef call_intrinsic(char *name, LLVMTypeRef *overloads, int noverloads,
                                   LLVMValueRef *args, int nargs) {
  unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
  if (!LLVMIntrinsicIsOverloaded(id))
    noverloads = 0;
  LLVMValueRef fn = LLVMGetIntrinsicDeclaration(mod, id, overloads, noverloads);
  return LLVMBuildCall2(bld, LLVMGlobalGetValueType(fn), fn, args, nargs, "");
}

static LLVMValueRef byte_gep(LLVMValueRef base, LLVMValueRef off) {
  return LLVMBuildGEP2(bld, i8_type(), base, &off, 1, "");
}

static LLVMValueRef field_addr(LLVMValueRef base, int off) {
  return off ? byte_gep(base, LLVMConstInt(i64_type(), off, false)) : base;
}

static LLVMValueRef i32_const(long v) {
  return LLVMConstInt(LLVMInt32TypeInContext(ctx), v, true);
}

static LLVMValueRef align_ptr_up(LLVMValueRef p, int align) {
  LLVMValueRef v = LLVMBuildPtrToInt(bld, p, i64_type(), "");
  v = LLVMBuildAdd(bld, v, LLVMConstInt(i64_type(), align - 1, false), "");
  v = LLVMBuildAnd(bld, v, LLVMConstInt(i64_type(), -(long)align, true), "");
  return LLVMBuildIntToPtr(bld, v, ptr_type(), "");
}

// Takes `size` bytes (rounded up to 8) from the overflow area whose pointer is
// stored at `slot`, first aligning it to `align`.
static LLVMValueRef va_take_stack(LLVMValueRef slot, int size, int align) {
  LLVMValueRef p = load(ptr_type(), slot, 8);
  if (align > 8)
    p = align_ptr_up(p, align);
  LLVMValueRef next = byte_gep(p, LLVMConstInt(i64_type(), (size + 7) / 8 * 8, false));
  store(next, slot, 8);
  return p;
}

static LLVMValueRef phi2(LLVMTypeRef ty, LLVMValueRef a, LLVMBasicBlockRef ab,
                         LLVMValueRef b, LLVMBasicBlockRef bb) {
  LLVMValueRef phi = LLVMBuildPhi(bld, ty, "");
  LLVMValueRef vals[2] = {a, b};
  LLVMBasicBlockRef bbs[2] = {ab, bb};
  LLVMAddIncoming(phi, vals, bbs, 2);
  return phi;
}

// AAPCS64 va_list: { void *__stack; void *__gr_top; void *__vr_top;
// int __gr_offs; int __vr_offs; }. The offsets count up from minus the size
// of the register save area to 0; a non-negative offset means the registers
// are used up and arguments come from the stack.
static LLVMValueRef va_arg_aarch64(LLVMValueRef ap, Type *ty) {
  int hfa_n = 0;
  Type *hfa = hfa_base(ty, &hfa_n);
  // Other aggregates larger than 16 bytes are passed by reference.
  bool indirect = !hfa && is_aggregate(ty) && ty->size > 16;
  int size = indirect ? 8 : ty->size;
  int align = indirect ? 8 : natural_align(ty);
  bool fp = hfa || (!indirect && is_flonum(ty));
  int offs_field = fp ? 28 : 24;
  int top_field = fp ? 16 : 8;
  // Each floating-point value (or HFA element) takes a 16-byte vector slot.
  int reg_bytes = fp ? 16 * (hfa ? hfa_n : 1) : (size + 7) / 8 * 8;

  LLVMBasicBlockRef maybe_reg = new_block("va.maybe_reg");
  LLVMBasicBlockRef in_reg = new_block("va.in_reg");
  LLVMBasicBlockRef on_stack = new_block("va.on_stack");
  LLVMBasicBlockRef end = new_block("va.end");

  LLVMValueRef offs_addr = field_addr(ap, offs_field);
  LLVMValueRef offs = load(LLVMInt32TypeInContext(ctx), offs_addr, 4);
  LLVMBuildCondBr(bld, LLVMBuildICmp(bld, LLVMIntSGE, offs, i32_const(0), ""), on_stack, maybe_reg);

  position(maybe_reg);
  // A 16-byte-aligned argument starts at an even register.
  if (!fp && align >= 16)
    offs = LLVMBuildAnd(bld, LLVMBuildAdd(bld, offs, i32_const(15), ""), i32_const(-16), "");
  LLVMValueRef next = LLVMBuildAdd(bld, offs, i32_const(reg_bytes), "");
  store(next, offs_addr, 4);
  LLVMBuildCondBr(bld, LLVMBuildICmp(bld, LLVMIntSLE, next, i32_const(0), ""), in_reg, on_stack);

  position(in_reg);
  LLVMValueRef top = load(ptr_type(), field_addr(ap, top_field), 8);
  LLVMValueRef reg_addr = byte_gep(top, LLVMBuildSExt(bld, offs, i64_type(), ""));
  if (hfa) {
    // Gather the elements from their vector slots into the aggregate's layout.
    LLVMValueRef tmp = entry_alloca(storage_type(ty), 16);
    for (int i = 0; i < hfa_n; i++)
      copy_mem(field_addr(tmp, i * hfa->size), hfa->align, field_addr(reg_addr, 16 * i), 16,
               hfa->size);
    reg_addr = tmp;
  }
  LLVMBuildBr(bld, end);

  position(on_stack);
  LLVMValueRef stack_addr = va_take_stack(field_addr(ap, 0), size, align >= 16 ? 16 : 8);
  LLVMBasicBlockRef on_stack_end = LLVMGetInsertBlock(bld);
  LLVMBuildBr(bld, end);

  position(end);
  LLVMValueRef addr = phi2(ptr_type(), reg_addr, in_reg, stack_addr, on_stack_end);
  return indirect ? load(ptr_type(), addr, 8) : addr;
}

// x86-64 SysV va_list element: { unsigned gp_offset; unsigned fp_offset;
// void *overflow_arg_area; void *reg_save_area; }. The register save area
// holds the 6 integer registers (48 bytes) followed by the 8 SSE registers
// (16 bytes each, up to offset 176).
static LLVMValueRef va_arg_x86_64(LLVMValueRef ap, Type *ty) {
  LLVMValueRef overflow = field_addr(ap, 8);
  X86Class cls[2] = {CLS_NONE, CLS_NONE};
  if (is_scalar(ty)) {
    if (ty->kind == TY_LDOUBLE)
      return va_take_stack(overflow, ty->size, ty->align);
    cls[0] = is_flonum(ty) ? CLS_SSE : CLS_INT;
    if (ty->size > 8)
      cls[1] = CLS_INT; // __int128
  } else if (!x86_eightbyte_classes(ty, cls)) {
    return va_take_stack(overflow, ty->size, ty->align);
  }
  int need_gp = (cls[0] == CLS_INT) + (cls[1] == CLS_INT);
  int need_fp = (cls[0] == CLS_SSE) + (cls[1] == CLS_SSE);

  LLVMBasicBlockRef in_reg = new_block("va.in_reg");
  LLVMBasicBlockRef in_mem = new_block("va.in_mem");
  LLVMBasicBlockRef end = new_block("va.end");

  LLVMTypeRef i32 = LLVMInt32TypeInContext(ctx);
  LLVMValueRef gp_addr = field_addr(ap, 0);
  LLVMValueRef fp_addr = field_addr(ap, 4);
  LLVMValueRef gp = load(i32, gp_addr, 4);
  LLVMValueRef fp = load(i32, fp_addr, 4);
  LLVMValueRef fits = LLVMConstInt(LLVMInt1TypeInContext(ctx), 1, false);
  if (need_gp)
    fits = LLVMBuildAnd(bld, fits, LLVMBuildICmp(bld, LLVMIntULE, gp, i32_const(48 - 8 * need_gp), ""), "");
  if (need_fp)
    fits = LLVMBuildAnd(bld, fits, LLVMBuildICmp(bld, LLVMIntULE, fp, i32_const(176 - 16 * need_fp), ""), "");
  LLVMBuildCondBr(bld, fits, in_reg, in_mem);

  position(in_reg);
  LLVMValueRef save = load(ptr_type(), field_addr(ap, 16), 8);
  LLVMValueRef gp_slot = byte_gep(save, LLVMBuildZExt(bld, gp, i64_type(), ""));
  LLVMValueRef fp_slot = byte_gep(save, LLVMBuildZExt(bld, fp, i64_type(), ""));
  LLVMValueRef reg_addr;
  if (!need_fp) {
    reg_addr = gp_slot; // integer eightbytes are contiguous
  } else if (!need_gp && cls[1] == CLS_NONE) {
    reg_addr = fp_slot;
  } else {
    // SSE eightbytes are 16 bytes apart, and a mixed aggregate draws from
    // both areas: gather the eightbytes into a temporary.
    reg_addr = entry_alloca(LLVMArrayType2(i8_type(), 16), 16);
    int gi = 0, fi = 0;
    for (int i = 0; i < 2 && cls[i] != CLS_NONE; i++) {
      LLVMValueRef src = cls[i] == CLS_INT ? field_addr(gp_slot, 8 * gi++)
                                           : field_addr(fp_slot, 16 * fi++);
      copy_mem(field_addr(reg_addr, 8 * i), 8, src, 8, 8);
    }
  }
  if (need_gp)
    store(LLVMBuildAdd(bld, gp, i32_const(8 * need_gp), ""), gp_addr, 4);
  if (need_fp)
    store(LLVMBuildAdd(bld, fp, i32_const(16 * need_fp), ""), fp_addr, 4);
  LLVMBuildBr(bld, end);

  position(in_mem);
  LLVMValueRef mem_addr = va_take_stack(overflow, ty->size, ty->align);
  LLVMBasicBlockRef in_mem_end = LLVMGetInsertBlock(bld);
  LLVMBuildBr(bld, end);

  position(end);
  return phi2(ptr_type(), reg_addr, in_reg, mem_addr, in_mem_end);
}

static LLVMValueRef gen_va_arg(Node *node) {
  Type *ty = node->ty;
  if (!is_scalar(ty) && !is_aggregate(ty))
    unsupported(node->tok, format("va_arg of type %s", type_name(ty)));
  if (is_aggregate(ty))
    check_aggregate_abi(ty, node->tok);

  LLVMValueRef ap = gen_value(node->lhs);
  LLVMValueRef addr = arch == ARCH_AARCH64 ? va_arg_aarch64(ap, ty) : va_arg_x86_64(ap, ty);
  // A struct or union value is its address.
  if (is_aggregate(ty))
    return addr;
  return load(scalar_type(ty), addr, ty->align < 8 ? ty->align : 8);
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
    if (node->ty->kind == TY_LDOUBLE)
      return ldouble_const(node->fval);
    if (is_flonum(node->ty))
      return LLVMConstReal(scalar_type(node->ty), (double)node->fval);
    if (!is_integer(node->ty))
      unsupported(node->tok, format("constant of type %s", type_name(node->ty)));
    return LLVMConstInt(scalar_type(node->ty), (unsigned long long)node->val, false);

  case ND_VAR:
    // A function designator's value is the function's address.
    if (node->var->ty->kind == TY_FUNC)
      return var_addr(node->var, node->tok);
    // The value of an array, struct or union is its address.
    if (is_memory(node->ty))
      return var_addr(node->var, node->tok);
    if (!is_scalar(node->ty))
      unsupported(node->tok, format("variable '%s' of type %s", node->var->name, type_name(node->ty)));
    return load(scalar_type(node->ty), var_addr(node->var, node->tok), known_align(node));

  case ND_DEREF:
    if (!is_address(node->lhs->ty))
      unsupported(node->tok, "dereference of a non-pointer");
    // Dereferencing a pointer to an array (e.g. a row of a 2D array), struct,
    // union or function yields that object, whose value is its address.
    if (is_memory(node->ty) || node->ty->kind == TY_FUNC)
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
    if (is_flonum(node->ty))
      return LLVMBuildFNeg(bld, gen_value(node->lhs), "");
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

  case ND_VA_START:
  case ND_VA_END: {
    LLVMValueRef ap = gen_value(node->lhs);
    LLVMTypeRef pty = ptr_type();
    call_intrinsic(node->kind == ND_VA_START ? "llvm.va_start" : "llvm.va_end", &pty, 1, &ap, 1);
    return NULL;
  }

  case ND_VA_COPY: {
    LLVMValueRef args[2] = {gen_value(node->lhs), gen_value(node->rhs)};
    LLVMTypeRef pty = ptr_type();
    call_intrinsic("llvm.va_copy", &pty, 1, args, 2);
    return NULL;
  }

  case ND_VA_ARG:
    return gen_va_arg(node);

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
