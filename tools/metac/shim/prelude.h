/* The GNU prelude (doc/GENERATOR_DESIGN.md "Front end"): included before
   everything, it turns the GNU C that pycparser can't parse into plain C.
   Dropping __attribute__ loses packed and aligned, so the AST is never
   trusted for sizes, offsets or bit positions. */
#define __attribute__(x)
#define __extension__
#define __restrict
#define __restrict__
#define __inline inline
#define __inline__ inline
#define __asm__(x)
#define __asm(x)
#define __volatile__(...)
#define __builtin_va_list int
#define _Float128 double
#define _Float64x double
#define _Float32 float
#define _Float64 double
#define _Float32x double
#define __float128 double
#define _Noreturn
#define __signed__ signed
#define __const const
#define __volatile volatile
#define __typeof__(...) int
#define typeof(...) int
#define __auto_type int
#define __alignof__(x) 8
#define __builtin_offsetof(t, m) ((unsigned long)&((t *)0)->m)
#define __builtin_va_arg(v, t) (*(t *)__ni_va_arg(v))
#define __builtin_types_compatible_p(a, b) 0
