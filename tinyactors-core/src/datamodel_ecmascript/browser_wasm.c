/*
 * What tinyactors_datamodel_ecmascript.wasm, a module for browsers, has in
 * place of an operating system (docs/adr/0001-packaging-and-releases.md,
 * decisions 9 and 20).
 *
 * The module's C library is wasi-libc, which reaches the system through
 * WASI functions it would import (wasi_snapshot_preview1). A browser has
 * none, and the module imports nothing: it defines them here instead, so
 * the linker resolves wasi-libc's calls inside the module.
 *
 * - The clock is the time the host last set with
 *   ta_datamodel_ecmascript_wasm_set_time() (0, the epoch, until then):
 *   QuickJS's Date, its performance clock and its random seed read it.
 * - Files do not exist: QuickJS writes only its debugging dumps, which go
 *   nowhere (every descriptor is bad).
 */
#include <tinyactors/datamodel_ecmascript.h>

#include <stdint.h>

#define WASI_ERRNO_SUCCESS 0
#define WASI_ERRNO_BADF 8

static uint64_t now_ns;

void ta_datamodel_ecmascript_wasm_set_time(double unix_milliseconds) {
    now_ns = unix_milliseconds > 0 ? (uint64_t)(unix_milliseconds * 1e6) : 0u;
}

/* The WASI functions wasi-libc calls, by the names its declarations give
 * them (wasi/api.h). Defined here, they are not imported. */
int32_t __imported_wasi_snapshot_preview1_clock_time_get(int32_t clock, int64_t precision, int32_t time);
int32_t __imported_wasi_snapshot_preview1_fd_close(int32_t fd);
int32_t __imported_wasi_snapshot_preview1_fd_fdstat_get(int32_t fd, int32_t stat);
int32_t __imported_wasi_snapshot_preview1_fd_seek(int32_t fd, int64_t offset, int32_t whence, int32_t position);
int32_t __imported_wasi_snapshot_preview1_fd_write(int32_t fd, int32_t iovs, int32_t count, int32_t written);

int32_t __imported_wasi_snapshot_preview1_clock_time_get(int32_t clock, int64_t precision, int32_t time) {
    (void)clock;
    (void)precision;
    *(uint64_t *)(uintptr_t)time = now_ns;
    return WASI_ERRNO_SUCCESS;
}

int32_t __imported_wasi_snapshot_preview1_fd_close(int32_t fd) {
    (void)fd;
    return WASI_ERRNO_BADF;
}

int32_t __imported_wasi_snapshot_preview1_fd_fdstat_get(int32_t fd, int32_t stat) {
    (void)fd;
    (void)stat;
    return WASI_ERRNO_BADF;
}

int32_t __imported_wasi_snapshot_preview1_fd_seek(int32_t fd, int64_t offset, int32_t whence, int32_t position) {
    (void)fd;
    (void)offset;
    (void)whence;
    (void)position;
    return WASI_ERRNO_BADF;
}

int32_t __imported_wasi_snapshot_preview1_fd_write(int32_t fd, int32_t iovs, int32_t count, int32_t written) {
    (void)fd;
    (void)iovs;
    (void)count;
    (void)written;
    return WASI_ERRNO_BADF;
}

/* zig 0.16 implements close() in its own libc layer (libzigc), which imports
 * fd_close under the linker's name for an import of a module's function,
 * "<name>|<module>". If a zig upgrade names another import so, the
 * release's check that the module imports nothing names it. */
int32_t zig_fd_close(int32_t fd) __asm__("fd_close|wasi_snapshot_preview1");
int32_t zig_fd_close(int32_t fd) {
    (void)fd;
    return WASI_ERRNO_BADF;
}
