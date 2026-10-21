// Struct shapes shared by struct_abi.c (compiled by chibicc) and
// struct_abi_helper.c (compiled by the host C compiler). Each one exercises a
// different calling-convention class on x86-64 SysV and AAPCS64.
struct S1 { char a; };                       // 1 byte
struct S3 { char a, b, c; };                 // odd size, one register
struct S8 { int a, b; };                     // one register
struct S12 { int a, b, c; };                 // two registers, partial tail
struct S16 { long a, b; };                   // two registers
struct S24 { long a, b, c; };                // memory (byval / by reference)
struct A16 { long a; } __attribute__((aligned(16))); // AAPCS64: i128
struct P5 { char c; int i; } __attribute__((packed)); // x86-64: memory class
union U8 { int i; char c[6]; };              // union, two-byte tail padding
