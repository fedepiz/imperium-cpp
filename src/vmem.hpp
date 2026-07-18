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

fn u8* reserve(usize size) {
    void* ptr = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return 0;
    return (u8*)ptr;
}

fn void commit(u8* base, usize size) {
    bool ok = mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
    (void)ok;
    ASSERT(ok);
}

fn void release(u8* base, usize size) { munmap(base, size); }

} // namespace vmem

#elif defined(_WIN32)

// Keep windows.h lean: this header is included into every root TU, so the
// less it leaks the better. NOMINMAX kills the min/max macros.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace vmem {

fn u8* reserve(usize size) {
    return (u8*)VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS); // null on failure (ZII)
}

fn void commit(u8* base, usize size) {
    void* ptr = VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE);
    (void)ptr;
    ASSERT(ptr);
}

fn void release(u8* base, usize size) {
    (void)size; // MEM_RELEASE frees the whole reservation; size must be 0
    VirtualFree(base, 0, MEM_RELEASE);
}

} // namespace vmem
#else
#error "vmem: unsupported platform"
#endif
