// Shapes shared by float_abi.c (chibicc) and float_abi_helper.c (host C
// compiler), covering floating-point classes of both calling conventions.
struct F2 { float a, b; };          // AAPCS64 HFA; x86-64 one SSE eightbyte
struct F3 { float a, b, c; };       // HFA of 3; x86-64 SSE, SSE
struct D2 { double a, b; };         // HFA; x86-64 SSE, SSE
struct D4 { double a, b, c, d; };   // HFA of 4 (32 bytes, still in registers on AAPCS64)
struct ID { int i; double d; };     // mixed: x86-64 INTEGER, SSE
struct DI { double d; long i; };    // mixed: x86-64 SSE, INTEGER
struct FI { float f; int i; };      // one eightbyte: INTEGER on x86-64; not an HFA
struct LD { long double x; };       // x86-64 memory (x87); AAPCS64 HFA of fp128
