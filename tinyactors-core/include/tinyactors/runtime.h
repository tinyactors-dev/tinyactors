/* tinyactors/runtime.h — optional OS conveniences. Contains no SCXML semantics.
 *
 * malloc-backed allocator, file loading and compile-time src resolution,
 * OS monotonic clock, and a system wrapper that grows by relocation when a
 * pool is exhausted.
 */
#ifndef TINYACTORS_RUNTIME_H
#define TINYACTORS_RUNTIME_H

#include <tinyactors/allocator.h>
#include <tinyactors/compile.h>
#include <tinyactors/system.h>

#ifdef __cplusplus
extern "C" {
#endif

TA_API ta_allocator ta_runtime_malloc(void);

/* Files and the clock need an operating system: every target but
 * freestanding wasm has them (src/runtime/runtime_hosted.c). */
#if !defined(__wasm__) || defined(__wasi__)

/* Load a whole file into an 8-aligned buffer (suitable for ta_image_open). */
TA_API ta_status ta_runtime_load_file(const char *path, void **bytes, size_t *length);
TA_API void ta_runtime_free_file(void *bytes, size_t length);

/* Monotonic clock in nanoseconds (pair with ticks_per_second = 1e9). */
TA_API ta_time ta_runtime_now_ns(void);

/* File-system resolver for ta_compile: src="" is read relative to the
 * directory of the referring file. Loaded texts are owned by the set. */
typedef struct ta_runtime_files ta_runtime_files;
TA_API ta_runtime_files *ta_runtime_files_new(void);
TA_API void ta_runtime_files_free(ta_runtime_files *files);
TA_API ta_status ta_runtime_resolve(void *files, ta_string_view base, ta_string_view source,
                                    ta_source *out);

/* Compile an .scxml file (or a <system> manifest if `manifest` != 0). */
TA_API ta_status ta_runtime_compile_file(const char *path, int manifest, ta_compile_result **out);

#endif

/* malloc'd system that relocates into a larger block on TA_RUN_EXHAUSTED. */
TA_API ta_status ta_runtime_system_new(const ta_image *image, const ta_system_config *config,
                                       const ta_datamodel_binding *datamodels,
                                       uint32_t datamodel_count, ta_system **out);
/* Like ta_system_run, but grows the system (updating *system) instead of
 * returning TA_RUN_EXHAUSTED, as long as memory is available. */
TA_API ta_run ta_runtime_run(ta_system **system, uint32_t budget);
/* Double every capacity now (e.g. when ta_system_spawn/send report
 * TA_ERROR_NO_MEMORY); updates *system. */
TA_API ta_status ta_runtime_system_grow(ta_system **system);
TA_API void ta_runtime_system_free(ta_system *system);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_RUNTIME_H */
