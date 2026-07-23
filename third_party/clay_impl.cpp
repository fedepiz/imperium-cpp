// clay is header-only: this TU instantiates its implementation once, so the
// object can live in third_party.a and our TUs include clay.h declarations only.
// Compiled as C++, never C: clay's packed enums are 1 byte in C++
// (enum : uint8_t) but 4 bytes in C on MSVC-ABI targets, where clang ignores
// __attribute__((packed)) on enums — so a C-compiled implementation disagrees
// with our C++ TUs on the layout of every enum-bearing struct and clay reads
// garbage. The implementation must be built in the same language as the TUs
// that include clay.h.
#define CLAY_IMPLEMENTATION
#include "clay/clay.h"
