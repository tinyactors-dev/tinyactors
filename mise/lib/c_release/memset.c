/*
 * The memset every native C release links in (root ADR 0001,
 * docs/adr/0001-packaging-and-releases.md, decision 16), in place of zig
 * 0.16's compiler-rt one, which is scalar: in tinyactors-core, spawning,
 * delivering and destroying actors took 24% longer with it than with
 * glibc's, and all of that was memset (memcpy from compiler-rt matched
 * glibc's). Ghostty ships a vectorized memset for the same reason
 * (ghostty-org/ghostty, src/quirks_memset.zig).
 *
 * It is compiled into each library's release object, where it takes the
 * place of compiler-rt's weak memset, and made local like the other memory
 * functions, so it serves only the library that contains it. It is not part
 * of any library's source: a source embedder links their own C library's.
 *
 * Every size is a few stores, never a byte loop (core's memsets are mostly
 * 8 to 15 bytes, then 256 to 511): two overlapping stores of 1, 2, 4 or 8
 * bytes up to 16; two overlapping 16-byte vector stores (clang's vector
 * extension: SSE2 on x86-64, NEON on arm64) up to 32; above that, 64 bytes
 * a round and one overlapping 16-byte store for the tail. no_builtin keeps
 * the compiler from turning any of it back into a call to memset.
 */
#include <stddef.h>
#include <stdint.h>

typedef unsigned char release_bytes16 __attribute__((vector_size(16), aligned(1)));
typedef uint64_t release_u64 __attribute__((aligned(1), may_alias));
typedef uint32_t release_u32 __attribute__((aligned(1), may_alias));
typedef uint16_t release_u16 __attribute__((aligned(1), may_alias));

__attribute__((visibility("hidden"), no_builtin("memset"))) void *memset(void *destination, int value, size_t count) {
    unsigned char *to = destination;
    unsigned char *end = to + count;
    unsigned char byte = (unsigned char)value;
    if (count < 16u) {
        uint64_t word = (uint64_t)byte * 0x0101010101010101u;
        if (count >= 8u) {
            *(release_u64 *)to = word;
            *(release_u64 *)(end - 8) = word;
        } else if (count >= 4u) {
            *(release_u32 *)to = (uint32_t)word;
            *(release_u32 *)(end - 4) = (uint32_t)word;
        } else if (count >= 2u) {
            *(release_u16 *)to = (uint16_t)word;
            *(release_u16 *)(end - 2) = (uint16_t)word;
        } else if (count == 1u) {
            *to = byte;
        }
        return destination;
    }
    release_bytes16 fill = (release_bytes16){ 0 } + byte;
    if (count <= 32u) {
        *(release_bytes16 *)to = fill;
        *(release_bytes16 *)(end - 16) = fill;
        return destination;
    }
    while ((size_t)(end - to) >= 64u) {
        *(release_bytes16 *)(to) = fill;
        *(release_bytes16 *)(to + 16) = fill;
        *(release_bytes16 *)(to + 32) = fill;
        *(release_bytes16 *)(to + 48) = fill;
        to += 64;
    }
    while ((size_t)(end - to) >= 16u) {
        *(release_bytes16 *)to = fill;
        to += 16;
    }
    if (to < end) {
        *(release_bytes16 *)(end - 16) = fill;
    }
    return destination;
}
