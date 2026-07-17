#pragma once
#include "core.hpp"

// Virtual memory: reserve address space without backing pages, then commit
// ranges as they're needed. Reserved-but-uncommitted memory costs nothing
// physical; committed pages arrive zero-filled from the OS.

namespace vmem {

u8*  reserve(usize size);           // null on failure (ZII)
void commit(u8* base, usize size);  // base and size must be page-aligned
void release(u8* base, usize size); // returns the whole reservation

} // namespace vmem

// The platform-specific implementations, each in its namespace block, guarded
// by platform flags.

#if defined(__APPLE__)

#include <sys/mman.h>

namespace vmem {

u8* reserve(usize size) {
    void* ptr = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return 0;
    return (u8*)ptr;
}

void commit(u8* base, usize size) {
    bool ok = mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
    (void)ok;
    ASSERT(ok);
}

void release(u8* base, usize size) { munmap(base, size); }

} // namespace vmem

#elif defined(_WIN32)
#error "vmem: windows implementation not written yet (VirtualAlloc reserve/commit)"
#else
#error "vmem: unsupported platform"
#endif
