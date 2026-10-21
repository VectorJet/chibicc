#ifndef __STDARG_H
#define __STDARG_H

#ifdef __chibicc_llvm__

// With the LLVM backend, the va_list layout is the target ABI's and the
// operations are builtins lowered by the backend.
#ifdef __aarch64__
typedef struct __va_list {
  void *__stack;
  void *__gr_top;
  void *__vr_top;
  int __gr_offs;
  int __vr_offs;
} va_list;
#else
typedef struct {
  unsigned int gp_offset;
  unsigned int fp_offset;
  void *overflow_arg_area;
  void *reg_save_area;
} __va_elem;

typedef __va_elem va_list[1];
#endif

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap) __builtin_va_end(ap)
#define va_arg(ap, ty) __builtin_va_arg(ap, ty)
#define va_copy(dest, src) __builtin_va_copy(dest, src)
#define __va_copy(dest, src) __builtin_va_copy(dest, src)

#else

typedef struct {
  unsigned int gp_offset;
  unsigned int fp_offset;
  void *overflow_arg_area;
  void *reg_save_area;
} __va_elem;

typedef __va_elem va_list[1];

#define va_start(ap, last) \
  do { *(ap) = *(__va_elem *)__va_area__; } while (0)

#define va_end(ap)

static void *__va_arg_mem(__va_elem *ap, int sz, int align) {
  void *p = ap->overflow_arg_area;
  if (align > 8)
    p = (p + 15) / 16 * 16;
  ap->overflow_arg_area = ((unsigned long)p + sz + 7) / 8 * 8;
  return p;
}

static void *__va_arg_gp(__va_elem *ap, int sz, int align) {
  if (ap->gp_offset >= 48)
    return __va_arg_mem(ap, sz, align);

  void *r = ap->reg_save_area + ap->gp_offset;
  ap->gp_offset += 8;
  return r;
}

static void *__va_arg_fp(__va_elem *ap, int sz, int align) {
  if (ap->fp_offset >= 112)
    return __va_arg_mem(ap, sz, align);

  void *r = ap->reg_save_area + ap->fp_offset;
  ap->fp_offset += 8;
  return r;
}

#define va_arg(ap, ty)                                                  \
  ({                                                                    \
    int klass = __builtin_reg_class(ty);                                \
    *(ty *)(klass == 0 ? __va_arg_gp(ap, sizeof(ty), _Alignof(ty)) :    \
            klass == 1 ? __va_arg_fp(ap, sizeof(ty), _Alignof(ty)) :    \
            __va_arg_mem(ap, sizeof(ty), _Alignof(ty)));                \
  })

#define va_copy(dest, src) ((dest)[0] = (src)[0])

#endif

#define __GNUC_VA_LIST 1
typedef va_list __gnuc_va_list;

#endif
