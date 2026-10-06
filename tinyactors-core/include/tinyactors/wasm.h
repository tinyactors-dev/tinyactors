/* tinyactors/wasm.h — the WebAssembly modules' own allocator.
 *
 * tinyactors.wasm and tinyactors_runtime.wasm import nothing: a host that
 * loads one supplies no memory management of its own
 * (docs/adr/0001-packaging-and-releases.md, decision 6). It places the bytes
 * it hands the library (a system's block, an image to open, a chart to
 * compile) in the module's linear memory with ta_wasm_alloc(), and gives a
 * block back with ta_wasm_free() and the size and alignment it asked for.
 * Alignments above 65536 are refused (NULL).
 */
#ifndef TINYACTORS_WASM_H
#define TINYACTORS_WASM_H

#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__wasm__)
TA_API void *ta_wasm_alloc(size_t size, size_t alignment);
TA_API void ta_wasm_free(void *pointer, size_t size, size_t alignment);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TINYACTORS_WASM_H */
