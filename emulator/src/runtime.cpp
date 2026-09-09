// The whole C++ runtime the WebAssembly build needs.
//
// The firmware core is freestanding: no heap, no exceptions, no RTTI, no
// standard library. What remains is what the compiler emits on its own:
// memcpy/memset for struct copies, the pure-virtual trap, and the deleting
// destructor's call to operator delete, which is never reached because no
// node is ever deleted (the pool destroys in place).

#include <stddef.h>
#include <stdint.h>

extern "C" {

void* memcpy(void* dst, const void* src, size_t n){
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    while (n--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, size_t n){
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (d < s){ while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}

void* memset(void* dst, int c, size_t n){
    uint8_t* d = static_cast<uint8_t*>(dst);
    while (n--) *d++ = static_cast<uint8_t>(c);
    return dst;
}

void __cxa_pure_virtual(){ __builtin_trap(); }

// Static destructors would be registered here; the page never exits.
int __cxa_atexit(void (*)(void*), void*, void*){ return 0; }

}

void operator delete(void*) noexcept { __builtin_trap(); }
void operator delete(void*, size_t) noexcept { __builtin_trap(); }
