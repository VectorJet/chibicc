// LLVM backend, selected with --backend=llvm.
//
// The backend lowers the AST produced by parse.c directly to LLVM IR through
// the LLVM-C API, verifies the module, and writes an object file with the LLVM
// target machine. The driver links objects with clang-19. Anything outside the
// supported subset is reported as a diagnostic. Nothing is silently dropped or
// approximated.
//
// Supported: integer and pointer scalars and fixed-size arrays as locals,
// parameters and globals (including string literals and global initializers
// that hold addresses of other globals or functions); direct calls; integer
// arithmetic, bitwise and comparison operators; pointer arithmetic, pointer
// difference and comparison; integer/pointer casts; short-circuit && and ||;
// ?:; casts between integer types and to/from bool; assignment and compound
// assignment (chibicc desugars the latter through ND_ADDR/ND_DEREF, which are
// handled here); if, for, while, do/while, switch (including GNU case ranges),
// goto and labels, break, continue and return; GNU statement expressions.
//
// Arrays are stored as byte arrays ([N x i8]); elements are accessed through
// byte-offset GEPs, which is what the frontend's pre-scaled pointer arithmetic
// produces.
//
// Not supported yet (diagnosed): floating point, structs and unions, VLAs,
// indirect calls, computed goto, thread-local variables, and anything else not
// listed above.

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

// Types that can live in memory: scalars and fixed-size arrays.
static bool is_storable(Type *ty) {
  return is_scalar(ty) || is_array(ty);
}

static LLVMTypeRef i8_type(void) {
  return LLVMInt8TypeInContext(ctx);
}

static LLVMTypeRef i64_type(void) {
  return LLVMInt64TypeInContext(ctx);
}

// LLVM type of an object's storage. Arrays are opaque byte arrays.
static LLVMTypeRef storage_type(Type *ty) {
  if (is_array(ty))
    return LLVMArrayType2(i8_type(), ty->size);
  return scalar_type(ty);
}

static LLVMTypeRef function_type(Type *fty, Token *tok) {
  Type *rt = fty->return_ty;
  LLVMTypeRef ret;
  if (rt->kind == TY_VOID) {
    ret = LLVMVoidTypeInContext(ctx);
  } else {
    ret = is_scalar(rt) ? scalar_type(rt) : NULL;
    if (!ret)
      unsupported(tok, format("return type %s", type_name(rt)));
  }

  int n = 0;
  for (Type *p = fty->params; p; p = p->next)
    n++;

  LLVMTypeRef *params = calloc(n ? n : 1, sizeof(LLVMTypeRef));
  int i = 0;
  for (Type *p = fty->params; p; p = p->next, i++) {
    params[i] = is_scalar(p) ? scalar_type(p) : NULL;
    if (!params[i])
      unsupported(tok, format("parameter type %s", type_name(p)));
  }
  return LLVMFunctionType(ret, params, n, fty->is_variadic);
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

  if (!requested || !strcmp(requested, host))
    return host;

  for (size_t i = 0; i < sizeof(known_targets) / sizeof(*known_targets); i++)
    if (!strcmp(requested, known_targets[i]))
      error("target '%s' is known but not enabled yet; this build targets only the host '%s'",
            requested, host);

  error("unknown target '%s' (host is '%s')", requested, host);
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
  if (is_array(var->ty))
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

  f = LLVMAddFunction(mod, fn->name, function_type(fn->ty, fn->tok ? fn->tok : tok));
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
  if (!is_void && !is_scalar(node->ty))
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
  LLVMValueRef phi = LLVMBuildPhi(bld, scalar_type(node->ty), "");
  LLVMValueRef vals[2] = {tv, ev};
  LLVMBasicBlockRef bbs[2] = {then_end, else_end};
  LLVMAddIncoming(phi, vals, bbs, 2);
  return phi;
}

static LLVMValueRef gen_call(Node *node) {
  if (node->lhs->kind != ND_VAR || node->lhs->var->ty->kind != TY_FUNC)
    unsupported(node->tok, "indirect call");
  if (node->pass_by_stack || node->ret_buffer)
    unsupported(node->tok, "struct argument or return value");

  LLVMValueRef fn = get_function(node->lhs->var, node->tok);
  LLVMTypeRef fty = LLVMGlobalGetValueType(fn);

  int n = 0;
  for (Node *a = node->args; a; a = a->next)
    n++;

  LLVMValueRef *args = calloc(n ? n : 1, sizeof(LLVMValueRef));
  Type *param = node->func_ty ? node->func_ty->params : NULL;
  int i = 0;
  for (Node *a = node->args; a; a = a->next, i++) {
    // Arrays only reach here as variadic arguments, which the frontend does
    // not convert; their value is already the decayed pointer.
    if (!is_scalar(a->ty) && !is_array(a->ty))
      unsupported(a->tok, format("argument of type %s", type_name(a->ty)));
    LLVMValueRef v = gen_value(a);
    if (param) {
      // Declared parameters were already converted by the frontend.
      param = param->next;
    } else if (is_integer(a->ty) && a->ty->size < 4) {
      // Default argument promotion for variadic and unprototyped arguments:
      // integers narrower than int are passed as int.
      v = int_coerce(v, scalar_type(ty_int), a->ty->is_unsigned);
    }
    args[i] = v;
  }

  LLVMValueRef r = LLVMBuildCall2(bld, fty, fn, args, n, "");
  if (node->ty->kind == TY_VOID)
    return NULL;
  return r;
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
    unsupported(node->tok, "struct or union member access");
  default:
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
    // The value of an array is the address of its first element.
    if (is_array(node->ty))
      return var_addr(node->var, node->tok);
    if (!is_scalar(node->ty))
      unsupported(node->tok, format("variable '%s' of type %s", node->var->name, type_name(node->ty)));
    return LLVMBuildLoad2(bld, scalar_type(node->ty), var_addr(node->var, node->tok), "");

  case ND_DEREF:
    if (!is_address(node->lhs->ty))
      unsupported(node->tok, "dereference of a non-pointer");
    // Dereferencing a pointer to an array (e.g. a row of a 2D array) yields
    // the array, whose value is its address.
    if (is_array(node->ty))
      return gen_value(node->lhs);
    if (!is_scalar(node->ty))
      unsupported(node->tok, format("dereference of %s", type_name(node->ty)));
    return LLVMBuildLoad2(bld, scalar_type(node->ty), gen_value(node->lhs), "");

  case ND_ADDR:
    return gen_addr(node->lhs);

  case ND_ASSIGN: {
    if (!is_scalar(node->lhs->ty))
      unsupported(node->tok, format("assignment to %s", type_name(node->lhs->ty)));
    LLVMValueRef addr = gen_addr(node->lhs);
    LLVMValueRef v = convert(gen_value(node->rhs), node->lhs->ty, node->rhs->ty, node->tok);
    LLVMBuildStore(bld, v, addr);
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
    if (is_array(var->ty))
      LLVMBuildMemSet(bld, addr, LLVMConstNull(i8_type()),
                      LLVMConstInt(i64_type(), var->ty->size, false), var->align);
    else
      LLVMBuildStore(bld, LLVMConstNull(scalar_type(var->ty)), addr);
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

  case ND_MEMBER:
    unsupported(node->tok, "struct or union member access");

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

static void gen_return(Node *node) {
  if (cur_ret_ty->kind == TY_VOID) {
    if (node->lhs)
      gen_expr(node->lhs);
    LLVMBuildRetVoid(bld);
    return;
  }

  if (!node->lhs) {
    LLVMBuildRet(bld, LLVMConstNull(scalar_type(cur_ret_ty)));
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
  local_binds = NULL;
  labels = NULL;

  LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, f, "entry");
  position(entry);

  // Every local gets a stack slot in the entry block. Only scalars and
  // fixed-size arrays are supported; other locals are diagnosed if they are
  // referenced.
  for (Obj *v = fn->locals; v; v = v->next) {
    if (!is_storable(v->ty))
      continue;
    LLVMValueRef slot = LLVMBuildAlloca(bld, storage_type(v->ty), "");
    if (v->align > 0)
      LLVMSetAlignment(slot, v->align);
    add_binding(&local_binds, v, slot);
  }

  int i = 0;
  for (Obj *p = fn->params; p; p = p->next, i++) {
    LLVMValueRef slot = find_binding(local_binds, p);
    if (!slot)
      unsupported(p->tok, format("parameter '%s' of type %s", p->name, type_name(p->ty)));
    LLVMBuildStore(bld, LLVMGetParam(f, i), slot);
  }

  gen_stmt(fn->body);

  // Falling off the end returns zero, as the legacy backend does.
  if (!is_terminated()) {
    if (cur_ret_ty->kind == TY_VOID)
      LLVMBuildRetVoid(bld);
    else
      LLVMBuildRet(bld, LLVMConstNull(scalar_type(cur_ret_ty)));
  }
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
