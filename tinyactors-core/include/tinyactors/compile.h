/* tinyactors/compile.h — SCXML parser, validator, static analysis, image linker,
 * and an XML writer for charts a binding builds.
 *
 * Usable offline (the `tac` CLI) or in-process (e.g. a Python binding that
 * loads .scxml at runtime). Unlike the runtime layers it allocates, through
 * a caller-supplied ta_allocator (malloc by default via ta_runtime).
 */
#ifndef TINYACTORS_COMPILE_H
#define TINYACTORS_COMPILE_H

#include <tinyactors/allocator.h>
#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_diagnostic {
    uint32_t severity; /* TA_DIAGNOSTIC_* */
    uint32_t code;
    uint32_t line, column;
    ta_string_view file;
    ta_string_view message;
} ta_diagnostic;
enum { TA_DIAGNOSTIC_NOTE = 0, TA_DIAGNOSTIC_WARNING = 1, TA_DIAGNOSTIC_ERROR = 2 };

typedef struct ta_source {
    ta_string_view name; /* used for diagnostics and src="" resolution */
    ta_string_view text;
} ta_source;

/* Resolves static src="" (invoke/data/script) at compile time. */
typedef ta_status (*ta_resolve_function)(void *context, ta_string_view base, ta_string_view source,
                                         ta_source *out);

/* Optional datamodel compile-time hook: validate expressions and emit an AOT
 * section (e.g. generated C, bytecode) keyed by the datamodel name. */
typedef struct ta_datamodel_compiler {
    const char *name;
    void *context;
    ta_status (*check)(void *context, uint32_t kind, ta_string_view source,
                       ta_diagnostic *diagnostic);
    ta_status (*emit_ahead_of_time)(void *context, const void *image, size_t length,
                                    ta_allocator *a, void **out, size_t *out_length);
} ta_datamodel_compiler;

enum { TA_COMPILE_STRIP_DEBUG = 1u << 0, TA_COMPILE_WERROR = 1u << 1 };
#define TA_COMPILE_MAX_CONTENT_NESTING 64u

typedef struct ta_compile_options {
    uint32_t size; /* sizeof(ta_compile_options); fields past `pad` are read only if covered */
    uint32_t flags;
    ta_allocator alloc;
    ta_resolve_function resolve;
    void *resolve_context;
    const ta_datamodel_compiler *datamodel_compilers;
    uint32_t datamodel_compiler_count;
    uint32_t pad;
    /* A flag the host sets asynchronously to stop compiling (e.g. a
     * database's pending-interrupt flag). Polled at least once per markup
     * token, state, transition and executable-content element; once it reads
     * nonzero the call returns TA_ERROR_ABORTED promptly with a "compilation
     * aborted" error diagnostic and no image. NULL: none. */
    const volatile int *abort_flag;
} ta_compile_options;

typedef struct ta_compile_result ta_compile_result;

/* Compile time and memory grow linearly with the total size of the
 * documents, and native stack use is bounded by constants. Limits (beyond
 * them a document is rejected): 200 levels of state nesting,
 * TA_COMPILE_MAX_CONTENT_NESTING levels of <if>/<foreach>, 65,535 event
 * descriptors or targets per transition, 64 dot-separated parts per event
 * name. At most 200 diagnostics are kept.
 *
 * One or more charts -> image (first chart is the default boot chart). */
TA_API ta_status ta_compile_charts(const ta_compile_options *opt, const ta_source *charts,
                                   uint32_t n, ta_compile_result **out);
/* System manifest (charts, boot actors, links, io_processors) -> image. */
TA_API ta_status ta_compile_system(const ta_compile_options *opt, const ta_source *manifest,
                                   ta_compile_result **out);

TA_API ta_string_view ta_compile_image(const ta_compile_result *r); /* bytes of the .tai */
TA_API uint32_t ta_compile_diagnostics(const ta_compile_result *r, const ta_diagnostic **out);
TA_API void ta_compile_free(ta_compile_result *r);

/* XML from calls, for charts and manifests a binding builds instead of
 * writing XML text itself: it escapes, indents two spaces per element level
 * and checks nesting, and knows no SCXML. Give the text to
 * ta_compile_charts / ta_compile_system, which validate it; their
 * diagnostics' lines and columns point into it. After an error, every later
 * call returns the same error.
 *
 * An attribute is quoted with ", or with ' when its value holds " and no ';
 * text is written where it is given, without indentation. */
typedef struct ta_xml_writer ta_xml_writer;

/* allocator NULL: the C library's malloc, as for ta_compile_options. */
TA_API ta_status ta_xml_writer_new(const ta_allocator *allocator, ta_xml_writer **out);
/* TA_ERROR_INVALID: not an XML name. TA_ERROR_STATE: a second root. */
TA_API ta_status ta_xml_writer_open(ta_xml_writer *writer, ta_string_view name);
/* TA_ERROR_STATE: no open element, or after its first child or text.
 * TA_ERROR_INVALID: not an XML name, or a control character in the value. */
TA_API ta_status ta_xml_writer_attribute(ta_xml_writer *writer, ta_string_view name,
                                         ta_string_view value);
/* TA_ERROR_STATE: no open element. TA_ERROR_INVALID: a control character. */
TA_API ta_status ta_xml_writer_text(ta_xml_writer *writer, ta_string_view text);
/* TA_ERROR_STATE: no open element. */
TA_API ta_status ta_xml_writer_close(ta_xml_writer *writer);
/* The document, valid until ta_xml_writer_free.
 * TA_ERROR_STATE: no root, or an element still open. */
TA_API ta_status ta_xml_writer_finish(ta_xml_writer *writer, ta_string_view *out);
TA_API void ta_xml_writer_free(ta_xml_writer *writer);

#ifdef __cplusplus
}
#endif
#endif /* TINYACTORS_COMPILE_H */
