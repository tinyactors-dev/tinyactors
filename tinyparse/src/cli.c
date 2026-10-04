/*
** cli.c -- the hosted pocketparse command-line shell.
**
** This is the ONLY file under src/ that may use libc. It is the hosted shell
** around the freestanding library: it reads argv, prints usage, the version,
** and the embedded manual, reads grammar and input files, drives the library,
** walks the concrete syntax tree through the public cursor API to print a tree
** dump, and renders diagnostics to stderr. The library itself never touches IO.
**
** The manual page is embedded as a C array in the generated header
** src/manpage_data.h, produced at build time by rendering pocketparse.1.md
** with mdc. `pocketparse man` prints those bytes verbatim.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "pocketparse.h"
#include "manpage_data.h"
#include "grammars_data.h"
#include "amalgamation_data.h"

/* ** A parsed (package ...) manifest, mirrored from package.c so the hosted
** shell can read a manifest without pulling in the freestanding internal.h. It
** names only public types. KEEP IN SYNC with the definition in package.c. */
typedef struct pocketparse_manifest {
    pocketparse_string name;
    pocketparse_string version;
    pocketparse_string extends;
    int                ok;
} pocketparse_manifest;

/* ** Library-private W59 hook (defined in engine.c, NOT in the public header):
** select the bytecode VM or the reference executor for parses on this engine.
** Declared here the way pocketparse_apply_manifest is, so the public API stays
** frozen while the CLI can flip the engine for POCKETPARSE_ENGINE=vm. */
void pp_engine_select_vm(pocketparse_engine *engine, int use_vm);

/* ** Select the actor-hosted bytecode VM for parses on this engine (W60). Declared
** here the same way, so the public API stays frozen while the CLI can flip the
** engine for POCKETPARSE_ENGINE=vm-ax. Defined in engine.c. */
void pp_engine_select_vm_actor(pocketparse_engine *engine, int use_vm_actor);

/* ** Force the rule memo on/off for parses on this engine (W67). Declared here
** the same way; the CLI maps POCKETPARSE_RULEMEMO to it. Defined in engine.c. */
void pp_engine_force_rule_memo(pocketparse_engine *engine, int mode);

/* ** Report whether the actor-hosted VM action ran for a parser (W60). Declared
** here so `run` can print the structural proof that the parse executed inside an
** ax actor when POCKETPARSE_TRACE_ACTOR is set. Defined in vmactor.c. */
int pp_parser_actor_ran(const pocketparse_parser *parser);

/* ** Parse a package.scm manifest (defined in package.c) and bind its params:
** onto the engine as compile-time parameters before any --param override. */
pocketparse_result pocketparse_apply_manifest(pocketparse_engine *engine,
                                              pocketparse_source manifest,
                                              pocketparse_manifest *out);

/* ** The host memory grant for the engine. A few megabytes is ample for the
** grammars and inputs the CLI handles; the library never allocates outside it.
** grow lets a stubborn parse spill into fresh malloc'd blocks rather than fail. */
#define POCKETPARSE_CLI_GRANT (16u * 1024u * 1024u)

/* ** grow callback: hand the library a fresh block when its arena is exhausted.
** It is never freed individually; the process exit reclaims it. */
static void *cli_grow(void *user, size_t need, size_t *got)
{
    (void)user;
    if (got != 0)
        *got = need;
    return malloc(need);
}

/* ** Wrap a NUL-terminated C string as a pocketparse_string. */
static pocketparse_string cli_text(const char *s)
{
    pocketparse_string out;
    out.pointer = (const uint8_t *)s;
    out.length  = (uint32_t)strlen(s);
    return out;
}

/* ** A growable, NUL-terminated character buffer backed by libc malloc. The CLI
** is hosted, so a path that outgrows any fixed buffer must still be built whole
** rather than truncated: callers append into this and it doubles its block as it
** fills. Like the rest of the CLI's allocations it is short-lived and leaked to
** the process on exit, matching the grant model in cli_grow. A push that cannot
** allocate sets `failed`, which the caller checks once at the end rather than per
** append. */
typedef struct cli_strbuf {
    char  *data;
    size_t length;   /* bytes used, not counting the trailing NUL */
    size_t capacity; /* bytes allocated, including room for the NUL */
    int    failed;
} cli_strbuf;

/* ** Initialise an empty buffer; the first append allocates. */
static void cli_strbuf_init(cli_strbuf *b)
{
    b->data     = NULL;
    b->length   = 0;
    b->capacity = 0;
    b->failed   = 0;
}

/* ** Ensure room for `extra` more bytes plus the trailing NUL, doubling the block
** geometrically. Sets `failed` and leaves the buffer usable (but unchanged) when
** the allocation fails. */
static void cli_strbuf_reserve(cli_strbuf *b, size_t extra)
{
    size_t need = b->length + extra + 1;
    if (need <= b->capacity)
        return;
    {
        size_t cap = (b->capacity == 0) ? 64 : b->capacity;
        char  *grown;
        while (cap < need)
            cap *= 2;
        grown = (char *)realloc(b->data, cap);
        if (grown == NULL) {
            b->failed = 1;
            return;
        }
        b->data     = grown;
        b->capacity = cap;
    }
}

/* ** Append `n` bytes, growing as needed. */
static void cli_strbuf_append(cli_strbuf *b, const char *bytes, size_t n)
{
    cli_strbuf_reserve(b, n);
    if (b->failed)
        return;
    memcpy(b->data + b->length, bytes, n);
    b->length += n;
    b->data[b->length] = '\0';
}

/* ** Append a NUL-terminated C string. */
static void cli_strbuf_append_cstr(cli_strbuf *b, const char *s)
{
    cli_strbuf_append(b, s, strlen(s));
}

/* ** Append one byte. */
static void cli_strbuf_append_byte(cli_strbuf *b, char c)
{
    cli_strbuf_append(b, &c, 1);
}

/* ** Duplicate a NUL-terminated string into a fresh malloc'd buffer, or NULL on
** allocation failure. Leaked to the process like the rest of the CLI's grant. */
static char *cli_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char  *copy = (char *)malloc(n);
    if (copy != NULL)
        memcpy(copy, s, n);
    return copy;
}

/* ** Read an entire file into a malloc'd buffer (defined below); the module
** resolver needs it before its definition. */
static char *read_all(const char *path, size_t *out_length);

/* ** Look up a package in the embedded registry by name (defined below); the
** module resolver needs it before its definition to fall back to a built-in
** base grammar for (extends NAME)/(import NAME) when the filesystem has none. */
struct pocketparse_embedded_package;
static const struct pocketparse_embedded_package *cli_embedded_lookup(
    const char *name);

/* ** The language tag (W62 injection) of the node under a cursor, an INTERNAL
** accessor defined in tree.c (declared in internal.h, which the CLI does not
** include). The public header is frozen, so the tag is reached through this
** forward declaration only -- --dump-tree prints it as a `[lang]' annotation so a
** composite multi-language tree shows which grammar parsed each subtree.
** PP_NO_SYMBOL (0xFFFFFFFF) means an untagged host-grammar node. */
uint32_t pp_tree_cursor_language_tag(const pocketparse_tree_cursor *cursor);
#define CLI_NO_LANGUAGE_TAG 0xFFFFFFFFu

/* ** The module-resolver context: the directory of the importing grammar, used
** to map a module NAME to a file relative to that directory. The directory is a
** heap-allocated, NUL-terminated string with no length cap, so a grammar reached
** through a long path still resolves its imports correctly. */
typedef struct cli_module_context {
    char *dir;
} cli_module_context;

/* ** Point ctx->dir at a fresh copy of the directory part of a grammar path
** (everything up to and including the last '/'), or at an empty string when the
** path has no slash (the module sits in the current directory). The copy is
** malloc'd and leaked to the process, like the rest of the CLI's grant. */
static void cli_module_context_init(cli_module_context *ctx, const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        ctx->dir = cli_strdup("");
        return;
    }
    {
        size_t n = (size_t)(slash - path) + 1; /* keep the trailing '/' */
        char  *dir = (char *)malloc(n + 1);
        if (dir != NULL) {
            memcpy(dir, path, n);
            dir[n] = '\0';
        }
        ctx->dir = dir;
    }
}

/* ** The filesystem-backed module resolver the CLI installs before compiling.
** It maps a dotted module NAME to a file relative to the importing grammar's
** directory: each '.' becomes a path separator and ".scm" is appended (e.g.
** `words` -> `<dir>/words.scm`, `a.b` -> `<dir>/a/b.scm`). The file is read into
** a malloc'd buffer (leaked to the process, like the rest of the CLI's grant)
** and returned to the library as bytes; the library never touches the disk.
**
** When no such file exists, the resolver falls back to the embedded registry,
** looking NAME up verbatim as a package name (e.g. `std.json'); this is what
** lets an embedded package's (extends std.json) find its built-in base with no
** filesystem at all. The filesystem is tried first, so a package on disk still
** SHADOWS the embedded copy, the same shadowing rule the grammar argument obeys. */
static pocketparse_result cli_module_resolve(void *user, pocketparse_string name,
                                             pocketparse_source *out_source)
{
    cli_module_context *ctx = (cli_module_context *)user;
    cli_strbuf path;
    uint32_t   i;
    char      *bytes;
    size_t     length = 0;

    if (ctx->dir == NULL)
        return POCKETPARSE_INVALID;

    /* Build "<dir>/<dotted/name>.scm" into a growable buffer: the path has no
    ** fixed length cap, so a module reached through a long directory or a deeply
    ** dotted name resolves rather than truncating. */
    cli_strbuf_init(&path);
    cli_strbuf_append_cstr(&path, ctx->dir);
    for (i = 0; i < name.length; i++) {
        char c = (char)name.pointer[i];
        cli_strbuf_append_byte(&path, (c == '.') ? '/' : c);
    }
    cli_strbuf_append_cstr(&path, ".scm");
    if (path.failed)
        return POCKETPARSE_OUT_OF_MEMORY;

    bytes = read_all(path.data, &length);
    if (bytes == NULL) {
        /* No file on disk: fall back to the embedded registry by package name.
        ** NAME is the dotted package name verbatim (e.g. `std.json'), so it is
        ** copied NUL-terminated and looked up as an embedded package. */
        cli_strbuf name_buf;
        const struct pocketparse_embedded_package *embedded;
        cli_strbuf_init(&name_buf);
        cli_strbuf_append(&name_buf, (const char *)name.pointer, name.length);
        if (name_buf.failed)
            return POCKETPARSE_OUT_OF_MEMORY;
        embedded = cli_embedded_lookup(name_buf.data ? name_buf.data : "");
        if (embedded == NULL)
            return POCKETPARSE_INVALID;
        out_source->name.pointer  = NULL;
        out_source->name.length   = 0;
        out_source->bytes.pointer = embedded->grammar_bytes;
        out_source->bytes.length  = (uint32_t)embedded->grammar_length;
        return POCKETPARSE_OK;
    }

    out_source->name.pointer    = NULL; /* diagnostics name unused for modules */
    out_source->name.length     = 0;
    out_source->bytes.pointer   = (const uint8_t *)bytes;
    out_source->bytes.length    = (uint32_t)length;
    return POCKETPARSE_OK;
}

/* ** Install the filesystem-backed module resolver for compiling a grammar at
** the given path: the resolver finds (import NAME)'d modules relative to that
** grammar's directory. The context is heap-allocated -- not a process-global
** static -- so the library holds no mutable static state and an embedder can
** install resolvers for several engines without them sharing one context. It
** outlives the synchronous compile that uses it and is leaked to the process on
** exit, like the rest of the CLI's grant. */
static void cli_install_module_resolver(pocketparse_engine *engine,
                                        const char *grammar_path)
{
    cli_module_context *ctx = (cli_module_context *)malloc(sizeof *ctx);
    if (ctx == NULL)
        return; /* without a resolver, imports simply fall back to the registry */
    cli_module_context_init(ctx, grammar_path);
    pocketparse_set_module_resolver(engine, cli_module_resolve, ctx);
}

/* ** The built-in character classes the CLI installs as CLASS bindings before it
** compiles or parses a grammar, so a grammar may write (class alpha) and the
** like without the host supplying its own. Each is a pocketparse_class_fn: it
** returns nonzero when the byte belongs to the class. They are ASCII-only and
** pure -- the matcher may probe a byte repeatedly while backtracking. */
static int cli_class_alpha(void *user, uint32_t byte)
{
    (void)user;
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

static int cli_class_digit(void *user, uint32_t byte)
{
    (void)user;
    return byte >= '0' && byte <= '9';
}

static int cli_class_alnum(void *user, uint32_t byte)
{
    return cli_class_alpha(user, byte) || cli_class_digit(user, byte);
}

static int cli_class_space(void *user, uint32_t byte)
{
    (void)user;
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/* ** Install one built-in CLASS binding by name onto the engine. */
static void cli_bind_class(pocketparse_engine *engine, const char *name,
                           pocketparse_class_fn fn)
{
    pocketparse_binding binding;
    binding.name     = cli_text(name);
    binding.kind     = POCKETPARSE_BIND_CLASS;
    binding.function = (void *)fn;
    binding.user     = NULL;
    pocketparse_bind(engine, &binding);
}

/* ** Install every built-in character class (alpha/digit/alnum/space) on the
** engine. Called by both `check` and `run` before the grammar is compiled, so a
** grammar's (class NAME) primitives resolve to these host functions. */
static void cli_install_builtin_classes(pocketparse_engine *engine)
{
    cli_bind_class(engine, "alpha", cli_class_alpha);
    cli_bind_class(engine, "digit", cli_class_digit);
    cli_bind_class(engine, "alnum", cli_class_alnum);
    cli_bind_class(engine, "space", cli_class_space);
}

/* ** Set one compile-time grammar parameter from a --param argument. The form is
** NAME[=VALUE]: a bare NAME sets the parameter truthy; NAME= or NAME=0 clears it,
** any other VALUE sets it. Parameters feed the grammar's (when PARAM: X)
** combinators. The name portion is the bytes before any '='. */
static void cli_set_param(pocketparse_engine *engine, const char *spec)
{
    const char        *eq = strchr(spec, '=');
    pocketparse_string name;
    int                value = 1;

    name.pointer = (const uint8_t *)spec;
    if (eq != NULL) {
        name.length = (uint32_t)(eq - spec);
        /* NAME= and NAME=0 clear the parameter; any other value sets it. */
        if (eq[1] == '\0' || (eq[1] == '0' && eq[2] == '\0'))
            value = 0;
    } else {
        name.length = (uint32_t)strlen(spec);
    }
    if (name.length == 0)
        return;
    pocketparse_set_param(engine, name, value);
}

/* ** True when a file at the path can be opened for reading. Used to probe for a
** package's manifest without depending on POSIX stat feature-test macros: the
** package format is addressed by directory, but a directory is recognised here
** by the manifest it must contain. */
static int cli_file_readable(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
        return 0;
    fclose(file);
    return 1;
}

/* ** True when the path ends in "package.scm" -- the manifest may be named
** directly instead of its containing directory. */
static int cli_is_manifest_path(const char *path)
{
    const char *base = strrchr(path, '/');
    base = (base == NULL) ? path : base + 1;
    return strcmp(base, "package.scm") == 0;
}

/* ** A grammar argument may name a package: a directory holding a package.scm,
** or the package.scm file itself. When it does, fill manifest_out with
** "<dir>/package.scm" and grammar_out with the sibling "<dir>/grammar.scm" and
** return 1; otherwise leave them empty and return 0, so the caller treats the
** argument as a plain grammar path. A directory is recognised by probing for the
** manifest inside it, which avoids a stat() and its feature-test macros. The
** buffers are sized for the same path budget the module resolver uses. */
static int cli_resolve_package(const char *arg, char **manifest_out,
                               char **grammar_out)
{
    cli_strbuf dir;
    size_t     n;

    *manifest_out = NULL;
    *grammar_out  = NULL;

    if (cli_is_manifest_path(arg)) {
        const char *slash = strrchr(arg, '/');
        n = (slash == NULL) ? 0 : (size_t)(slash - arg) + 1; /* keep trailing '/' */
    } else {
        /* Treat the argument as a directory and probe for its manifest. */
        cli_strbuf  probe;
        const char *sep = "/";
        size_t      len = strlen(arg);
        int         readable;
        if (len > 0 && arg[len - 1] == '/')
            sep = "";
        cli_strbuf_init(&probe);
        cli_strbuf_append_cstr(&probe, arg);
        cli_strbuf_append_cstr(&probe, sep);
        cli_strbuf_append_cstr(&probe, "package.scm");
        if (probe.failed)
            return 0;
        readable = cli_file_readable(probe.data);
        free(probe.data);
        if (!readable)
            return 0;
        n = len;
    }

    /* Build the directory prefix with a single trailing separator so the joins
    ** below are well-formed; the buffer has no path-length cap. */
    cli_strbuf_init(&dir);
    cli_strbuf_append(&dir, arg, n);
    if (dir.length > 0 && dir.data[dir.length - 1] != '/')
        cli_strbuf_append_byte(&dir, '/');
    if (dir.failed)
        return 0;

    {
        cli_strbuf manifest;
        cli_strbuf grammar;
        cli_strbuf_init(&manifest);
        cli_strbuf_append(&manifest, dir.data ? dir.data : "", dir.length);
        cli_strbuf_append_cstr(&manifest, "package.scm");
        cli_strbuf_init(&grammar);
        cli_strbuf_append(&grammar, dir.data ? dir.data : "", dir.length);
        cli_strbuf_append_cstr(&grammar, "grammar.scm");
        free(dir.data);
        if (manifest.failed || grammar.failed) {
            free(manifest.data);
            free(grammar.data);
            return 0;
        }
        *manifest_out = manifest.data;
        *grammar_out  = grammar.data;
    }
    return 1;
}

/* ** Look up a package name (e.g. "std.json") in the embedded registry baked
** into the binary by build.sh::gen_grammars. Returns the matching entry, or NULL
** when no embedded package carries that name. The registry is the fallback used
** when no filesystem search path resolves the name; a filesystem package found
** first SHADOWS the embedded copy, so this is consulted only after that fails. */
static const pocketparse_embedded_package *cli_embedded_lookup(const char *name)
{
    const pocketparse_embedded_package *entry;
    for (entry = pocketparse_embedded_packages; entry->name != NULL; entry++)
        if (strcmp(entry->name, name) == 0)
            return entry;
    return NULL;
}

/* ** A resolved grammar source: either a filesystem path to read, or bytes from
** the embedded registry. When `embedded' is non-NULL the grammar/manifest bytes
** come from it and the path fields are unused for IO; otherwise grammar_path
** points at a file to read and manifest_path (when is_package) at its manifest. */
typedef struct cli_grammar_source {
    const char                         *grammar_path;
    char                               *manifest_path;
    char                               *package_grammar;
    int                                 is_package;
    const pocketparse_embedded_package *embedded;
} cli_grammar_source;

/* ** Resolve a grammar argument to a source, honouring the shadowing rule: the
** filesystem wins. If the argument names a filesystem package directory or a
** package.scm, use it. Otherwise, if the argument is directly readable as a
** file, treat it as a plain grammar path. Only when neither holds do we fall
** back to the embedded registry, looking the argument up as a package name. The
** returned struct drives both `check' and `run'. */
static void cli_resolve_grammar_source(const char *arg, cli_grammar_source *out)
{
    out->grammar_path = arg;
    out->manifest_path   = NULL;
    out->package_grammar = NULL;
    out->is_package = 0;
    out->embedded   = NULL;

    if (cli_resolve_package(arg, &out->manifest_path, &out->package_grammar)) {
        out->is_package   = 1;
        out->grammar_path = out->package_grammar;
        return;
    }
    if (cli_file_readable(arg))
        return; /* a plain grammar file on disk */

    /* No filesystem package or file: fall back to the embedded registry. */
    out->embedded = cli_embedded_lookup(arg);
}

/* ** Apply an embedded package's manifest to the engine, binding its params:
** before any --param override, mirroring cli_apply_package for the filesystem
** case. The manifest bytes come straight from the registry, so no IO occurs. */
static pocketparse_result cli_apply_embedded(pocketparse_engine *engine,
                                             const pocketparse_embedded_package *pkg)
{
    pocketparse_manifest manifest;
    pocketparse_source   source;
    pocketparse_result   result;

    source.name          = cli_text(pkg->name);
    source.bytes.pointer = pkg->package_bytes;
    source.bytes.length  = (uint32_t)pkg->package_length;

    result = pocketparse_apply_manifest(engine, source, &manifest);
    if (result != POCKETPARSE_OK) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "error: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
    }
    return result;
}

/* ** Read a package's manifest and bind its params: onto the engine, BEFORE any
** --param the command line supplies, so the command line overrides a manifest
** default. The manifest bytes are read into a leaked buffer (like the rest of
** the CLI's grant). Returns POCKETPARSE_OK on success; on failure it has already
** printed an error to stderr. The manifest's identity fields are discarded here
** -- the resolver needs only the bound params and the sibling grammar.scm. */
static pocketparse_result cli_apply_package(pocketparse_engine *engine,
                                            const char *manifest_path)
{
    pocketparse_manifest manifest;
    pocketparse_source   source;
    pocketparse_result   result;
    char  *bytes;
    size_t length = 0;

    bytes = read_all(manifest_path, &length);
    if (bytes == NULL) {
        fprintf(stderr, "pocketparse: error: cannot read manifest '%s'\n",
                manifest_path);
        return POCKETPARSE_INVALID;
    }
    source.name          = cli_text(manifest_path);
    source.bytes.pointer = (const uint8_t *)bytes;
    source.bytes.length  = (uint32_t)length;

    result = pocketparse_apply_manifest(engine, source, &manifest);
    if (result != POCKETPARSE_OK) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "error: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
    }
    return result;
}

/* ** Print the usage summary. Contains the words "pocketparse" and "usage"
** that the test suite checks for. */
static void print_usage(FILE *stream)
{
    fprintf(stream,
        "pocketparse %s\n"
        "usage: pocketparse <command> [arguments]\n"
        "\n"
        "commands:\n"
        "  check <grammar>                 validate a grammar\n"
        "  run <grammar> --input <file> [--dump-tree] [--chunk N] [--trace]\n"
        "                                  parse input against a grammar\n"
        "  pack <grammar.scm> --out <file.ppg>\n"
        "                                  compile and write a binary grammar\n"
        "  c <grammar.scm> --out <file.c> [--bundle|--parser|--parser-lib] [--name P]\n"
        "                                  emit C source (module or standalone)\n"
        "  linearize <grammar> --input <file>\n"
        "                                  print the tree's ENTER/LEAF/EXIT stream\n"
        "  query <grammar> <query.scm> --input <file>\n"
        "                                  match a query and print its captures\n"
        "  node-types <grammar>            print the node-types schema as JSON\n"
        "  init <dir> [--amalgamation|--lib] [--grammar NAME]\n"
        "                                  seed a new project\n"
        "  vendor <dir> [--amalgamation|--lib]\n"
        "                                  drop or refresh the engine source\n"
        "  grammars                        list the embedded standard grammars\n"
        "  man                             print the manual page\n"
        "  version                         print the version\n"
        "  --help                          print this usage summary\n",
        pocketparse_version_string());
}

/* ** Print the embedded manual page to stdout. */
static int command_man(void)
{
    fwrite(pocketparse_manpage, 1, pocketparse_manpage_length, stdout);
    return 0;
}

/* ** Print the program name and version. */
static int command_version(void)
{
    printf("pocketparse %s\n", pocketparse_version_string());
    return 0;
}

/* ** grammars: list the embedded standard packages, one name per line, in
** registry order (e.g. `std.json'). These are the grammars resolvable with no
** filesystem present, baked into the binary by build.sh::gen_grammars. */
static int command_grammars(void)
{
    const pocketparse_embedded_package *entry;
    for (entry = pocketparse_embedded_packages; entry->name != NULL; entry++)
        printf("%s\n", entry->name);
    return 0;
}

/* ** Read an entire file (or stdin when path is "-") into a malloc'd buffer.
** Writes the length and returns the buffer, or NULL on error. */
static char *read_all(const char *path, size_t *out_length)
{
    FILE  *file;
    char  *buffer = NULL;
    size_t length = 0, capacity = 0;
    int    own = 0;

    if (strcmp(path, "-") == 0) {
        file = stdin;
    } else {
        file = fopen(path, "rb");
        if (file == NULL)
            return NULL;
        own = 1;
    }

    for (;;) {
        size_t got;
        if (length + 65536 > capacity) {
            size_t nc = capacity == 0 ? 65536 : capacity * 2;
            char *grown = realloc(buffer, nc);
            if (grown == NULL) {
                free(buffer);
                if (own) fclose(file);
                return NULL;
            }
            buffer = grown;
            capacity = nc;
        }
        got = fread(buffer + length, 1, 65536, file);
        length += got;
        if (got < 65536)
            break;
    }
    if (own)
        fclose(file);
    *out_length = length;
    return buffer;
}

/* ** Open an engine over a freshly allocated grant. Returns NULL on failure;
** the grant block is intentionally leaked to the process. */
static pocketparse_engine *open_engine(void)
{
    pocketparse_configuration configuration;
    void *block = malloc(POCKETPARSE_CLI_GRANT);
    if (block == NULL)
        return NULL;
    memset(&configuration, 0, sizeof configuration);
    configuration.memory.base          = block;
    configuration.memory.size          = POCKETPARSE_CLI_GRANT;
    configuration.memory.grow          = cli_grow;
    configuration.memory.user          = NULL;
    configuration.memory.chunk_minimum = 0;
    configuration.flags                = 0;
    {
        pocketparse_engine *engine = pocketparse_open(configuration);
        /* The actor-hosted bytecode VM is the DEFAULT engine, set in
        ** pocketparse_open itself (W64), so a parse genuinely runs on ax for
        ** everyone -- the CLI, the benchmark harness, any embedder -- with no flag
        ** set. POCKETPARSE_ENGINE names an explicit alternative:
        **   unset / "vm-ax"  -> the bytecode VM inside an ax actor (the default; W60)
        **   "vm"             -> the SAME VM in process, no actor (W59)
        ** W14-5 retired the recursive-descent IR interpreter, so the bytecode VM is
        ** the only engine. The former "recursive" selector now maps to the in-process VM
        ** (the closest non-actor engine) rather than naming a deleted executor; any
        ** other unrecognised value leaves the library's actor default in place. This
        ** is the only place the env var is read; the library itself never touches the
        ** environment. The vm-ax grant-too-small fallback to the in-process VM still
        ** applies underneath, so the default never breaks a parse on a constrained
        ** grant. */
        if (engine != NULL) {
            const char *which = getenv("POCKETPARSE_ENGINE");
            if (which != NULL &&
                (strcmp(which, "vm") == 0 || strcmp(which, "recursive") == 0)) {
                /* The in-process VM: clear the actor default, set the plain VM.
                ** "recursive" is accepted as a deprecated alias (its recursive
                ** executor is gone) so old invocations still pick a real engine. */
                pp_engine_select_vm_actor(engine, 0);
                pp_engine_select_vm(engine, 1);
            } else {
                /* The default actor path, already set by pocketparse_open. */
                pp_engine_select_vm_actor(engine, 1);
            }
            /* POCKETPARSE_RULEMEMO=always/off forces the rule memo on or off, so
            ** the differential can exercise its replay path on the test corpus
            ** (which is too small to wake the adaptive default). */
            {
                const char *rm = getenv("POCKETPARSE_RULEMEMO");
                if (rm != NULL && strcmp(rm, "always") == 0)
                    pp_engine_force_rule_memo(engine, 1);
                else if (rm != NULL && strcmp(rm, "off") == 0)
                    pp_engine_force_rule_memo(engine, 2);
            }
        }
        return engine;
    }
}

/* ** Render a diagnostic to stderr in a simple rustc-ish shape. The message is
** authored to contain the word "error" so the suite's err-contains holds. */
static void render_diagnostic(const pocketparse_diagnostic *diag)
{
    fprintf(stderr, "%.*s", (int)diag->message.length,
            (const char *)diag->message.pointer);
    if (diag->code.length > 0)
        fprintf(stderr, " [%.*s]", (int)diag->code.length,
                (const char *)diag->code.pointer);
    fputc('\n', stderr);
    if (diag->help.length > 0)
        fprintf(stderr, "  help: %.*s\n", (int)diag->help.length,
                (const char *)diag->help.pointer);
}

/* ** check <grammar>: compile the grammar; exit 0 if OK, else print the fault
** as an error to stderr and exit 1. */
static int command_check(int argc, char **argv)
{
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar = NULL;
    pocketparse_source   source;
    pocketparse_result   result;
    char  *grammar_bytes;
    size_t grammar_length;
    cli_grammar_source   src;

    if (argc < 1) {
        fprintf(stderr, "pocketparse: error: check needs a grammar path\n");
        return 1;
    }
    /* Resolve the argument: a filesystem package or file wins; failing both, the
    ** embedded registry is consulted (a path always SHADOWS the embedded copy). */
    cli_resolve_grammar_source(argv[0], &src);

    if (src.embedded != NULL) {
        source.name          = cli_text(src.embedded->name);
        source.bytes.pointer = src.embedded->grammar_bytes;
        source.bytes.length  = (uint32_t)src.embedded->grammar_length;
    } else {
        grammar_bytes = read_all(src.grammar_path, &grammar_length);
        if (grammar_bytes == NULL) {
            fprintf(stderr, "pocketparse: error: cannot read grammar '%s'\n",
                    src.grammar_path);
            return 1;
        }
        source.name          = cli_text(src.grammar_path);
        source.bytes.pointer = (const uint8_t *)grammar_bytes;
        source.bytes.length  = (uint32_t)grammar_length;
    }

    engine = open_engine();
    if (engine == NULL) {
        fprintf(stderr, "pocketparse: error: out of memory\n");
        return 1;
    }

    cli_install_builtin_classes(engine);
    cli_install_module_resolver(engine, src.grammar_path);
    /* Bind the manifest's params: first, then let a --param override them. */
    if (src.embedded != NULL) {
        if (cli_apply_embedded(engine, src.embedded) != POCKETPARSE_OK)
            return 1;
    } else if (src.is_package &&
               cli_apply_package(engine, src.manifest_path) != POCKETPARSE_OK) {
        return 1;
    }
    {
        int i;
        for (i = 1; i < argc; i++)
            if (strcmp(argv[i], "--param") == 0 && i + 1 < argc)
                cli_set_param(engine, argv[++i]);
    }
    result = pocketparse_compile_scheme(engine, source, &grammar);
    if (result != POCKETPARSE_OK) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "error: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        return 1;
    }
    return 0;
}

/* ** node-types <grammar-or-package>: compile the grammar (reusing the same
** resolver `check' and `run' use, so a package directory or an embedded name
** works too) and print its node-types schema as JSON to stdout. The schema --
** node kinds, their fields, and the types each field may hold -- is built by the
** freestanding library; this hosted shell only resolves the argument, drives the
** compile, and writes the returned bytes out. Exit 0 on success, 1 on any
** failure. */
static int command_node_types(int argc, char **argv)
{
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar = NULL;
    pocketparse_source   source;
    pocketparse_result   result;
    pocketparse_string   json;
    char  *grammar_bytes;
    size_t grammar_length;
    cli_grammar_source   src;

    if (argc < 1) {
        fprintf(stderr, "pocketparse: error: node-types needs a grammar path\n");
        return 1;
    }
    /* Resolve the argument: a filesystem package or file wins; failing both, the
    ** embedded registry is consulted (a path always SHADOWS the embedded copy). */
    cli_resolve_grammar_source(argv[0], &src);

    if (src.embedded != NULL) {
        source.name          = cli_text(src.embedded->name);
        source.bytes.pointer = src.embedded->grammar_bytes;
        source.bytes.length  = (uint32_t)src.embedded->grammar_length;
    } else {
        grammar_bytes = read_all(src.grammar_path, &grammar_length);
        if (grammar_bytes == NULL) {
            fprintf(stderr, "pocketparse: error: cannot read grammar '%s'\n",
                    src.grammar_path);
            return 1;
        }
        source.name          = cli_text(src.grammar_path);
        source.bytes.pointer = (const uint8_t *)grammar_bytes;
        source.bytes.length  = (uint32_t)grammar_length;
    }

    engine = open_engine();
    if (engine == NULL) {
        fprintf(stderr, "pocketparse: error: out of memory\n");
        return 1;
    }

    cli_install_builtin_classes(engine);
    cli_install_module_resolver(engine, src.grammar_path);
    /* Bind the manifest's params: first, then let a --param override them. */
    if (src.embedded != NULL) {
        if (cli_apply_embedded(engine, src.embedded) != POCKETPARSE_OK)
            return 1;
    } else if (src.is_package &&
               cli_apply_package(engine, src.manifest_path) != POCKETPARSE_OK) {
        return 1;
    }
    {
        int i;
        for (i = 1; i < argc; i++)
            if (strcmp(argv[i], "--param") == 0 && i + 1 < argc)
                cli_set_param(engine, argv[++i]);
    }

    result = pocketparse_compile_scheme(engine, source, &grammar);
    if (result != POCKETPARSE_OK) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "error: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        return 1;
    }

    result = pocketparse_emit_node_types(engine, grammar, &json);
    if (result != POCKETPARSE_OK) {
        fprintf(stderr, "pocketparse: error: cannot emit node types\n");
        return 1;
    }
    fwrite(json.pointer, 1, json.length, stdout);
    return 0;
}

/* ** analyze-tailcalls <grammar>: lower the grammar to bytecode and report how
** many rule CALLs sit in tail position and what fraction of all CALLs that is
** (the W14-6 tail-call analysis). It resolves the grammar exactly like
** node-types, compiles it, lowers its start rule with pp_compile_program (which
** marks the tail-position CALLs), and prints the counts. Reproducible evidence:
** the per-grammar numbers that drive the bytecode tail-call design come from this
** command, not a throwaway script. */
static int command_analyze_tailcalls(int argc, char **argv)
{
    /* Forward-declared internal entry (lives in vmcompile.c). It takes only public
    ** types plus plain uint32_t out-params, so cli.c needs no internal.h -- which
    ** matters for the amalgamation, where the CLI is a separate translation unit
    ** that cannot see the private header. */
    extern void pp_grammar_tail_call_counts(const pocketparse_grammar *grammar,
                                            uint32_t *out_total, uint32_t *out_tail);
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar = NULL;
    pocketparse_source   source;
    pocketparse_result   result;
    cli_grammar_source   src;
    uint32_t             total = 0, tail = 0;
    char  *grammar_bytes;
    size_t grammar_length;

    if (argc < 1) {
        fprintf(stderr, "pocketparse: error: analyze-tailcalls needs a grammar\n");
        return 1;
    }
    cli_resolve_grammar_source(argv[0], &src);
    if (src.embedded != NULL) {
        source.name          = cli_text(src.embedded->name);
        source.bytes.pointer = src.embedded->grammar_bytes;
        source.bytes.length  = (uint32_t)src.embedded->grammar_length;
    } else {
        grammar_bytes = read_all(src.grammar_path, &grammar_length);
        if (grammar_bytes == NULL) {
            fprintf(stderr, "pocketparse: error: cannot read grammar '%s'\n",
                    src.grammar_path);
            return 1;
        }
        source.name          = cli_text(src.grammar_path);
        source.bytes.pointer = (const uint8_t *)grammar_bytes;
        source.bytes.length  = (uint32_t)grammar_length;
    }

    engine = open_engine();
    if (engine == NULL) {
        fprintf(stderr, "pocketparse: error: out of memory\n");
        return 1;
    }
    cli_install_builtin_classes(engine);
    cli_install_module_resolver(engine, src.grammar_path);
    if (src.embedded != NULL) {
        if (cli_apply_embedded(engine, src.embedded) != POCKETPARSE_OK)
            return 1;
    } else if (src.is_package &&
               cli_apply_package(engine, src.manifest_path) != POCKETPARSE_OK) {
        return 1;
    }
    {
        int i;
        for (i = 1; i < argc; i++)
            if (strcmp(argv[i], "--param") == 0 && i + 1 < argc)
                cli_set_param(engine, argv[++i]);
    }

    result = pocketparse_compile_scheme(engine, source, &grammar);
    if (result != POCKETPARSE_OK) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "error: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        return 1;
    }

    pp_grammar_tail_call_counts(grammar, &total, &tail);

    {
        double pct = total > 0 ? (100.0 * (double)tail / (double)total) : 0.0;
        printf("%s\ttail=%u\ttotal=%u\tfraction=%.1f%%\n",
               argv[0], (unsigned)tail, (unsigned)total, pct);
    }
    return 0;
}

/* ** Decide whether a grammar path/bytes name a packed binary image rather than
** .scm source. A path ending in ".ppg" or a buffer opening with the "PPG1"
** magic is treated as binary; everything else is .scm source. The magic check
** lets a renamed or piped image still load correctly. */
static int looks_like_packed(const char *path, const char *bytes, size_t length)
{
    size_t n = strlen(path);
    if (n >= 4 && strcmp(path + n - 4, ".ppg") == 0)
        return 1;
    if (length >= 4 && bytes[0] == 'P' && bytes[1] == 'P' &&
        bytes[2] == 'G' && bytes[3] == '1')
        return 1;
    return 0;
}

/* ** Turn a grammar path into a grammar handle: .ppg images load through
** pocketparse_load_binary_grammar, .scm sources compile through
** pocketparse_compile_scheme. The resulting grammar parses identically either
** way. Returns POCKETPARSE_OK and writes *out_grammar, or prints the fault to
** stderr and returns the failing result. */
static pocketparse_result load_grammar(pocketparse_engine *engine,
                                       const char *path, char *bytes,
                                       size_t length,
                                       pocketparse_grammar **out_grammar)
{
    pocketparse_source source;
    pocketparse_result result;

    source.name          = cli_text(path);
    source.bytes.pointer = (const uint8_t *)bytes;
    source.bytes.length  = (uint32_t)length;

    if (looks_like_packed(path, bytes, length))
        result = pocketparse_load_binary_grammar(engine, source, out_grammar);
    else
        result = pocketparse_compile_scheme(engine, source, out_grammar);

    if (result != POCKETPARSE_OK) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "error: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
    }
    return result;
}

/* ** pack <grammar.scm> --out <file.ppg>: compile the .scm, serialize the
** compiled grammar to the binary format, and write the bytes to the output
** file. IO is fine here -- the CLI is the hosted shell -- while the library's
** serializer stays freestanding. */
static int command_pack(int argc, char **argv)
{
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar = NULL;
    pocketparse_result   result;
    pocketparse_string   packed;
    char  *grammar_bytes = NULL;
    size_t grammar_length = 0;
    const char *out_path = NULL;
    cli_grammar_source src;
    FILE  *out;
    int    i;

    if (argc < 1) {
        fprintf(stderr, "pocketparse: error: pack needs a grammar path\n");
        return 1;
    }
    /* Resolve the argument exactly as `run' does: a filesystem package or file
    ** wins, then the embedded registry. This lets `pack std.tsx' (or a package
    ** directory whose grammar.scm `extends' a base) pack the fully-resolved
    ** grammar, instead of only self-contained .scm files. */
    cli_resolve_grammar_source(argv[0], &src);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out_path = argv[++i];
        /* --param NAME[=VALUE] is applied after the engine opens, below. */
    }
    if (out_path == NULL) {
        fprintf(stderr, "pocketparse: error: pack needs --out\n");
        return 1;
    }

    if (src.embedded == NULL) {
        grammar_bytes = read_all(src.grammar_path, &grammar_length);
        if (grammar_bytes == NULL) {
            fprintf(stderr, "pocketparse: error: cannot read grammar '%s'\n",
                    src.grammar_path);
            return 1;
        }
    }
    engine = open_engine();
    if (engine == NULL) {
        fprintf(stderr, "pocketparse: error: out of memory\n");
        return 1;
    }

    cli_install_builtin_classes(engine);
    cli_install_module_resolver(engine, src.grammar_path);
    /* Bind the manifest's params: first, then let a --param override them. */
    if (src.embedded != NULL) {
        if (cli_apply_embedded(engine, src.embedded) != POCKETPARSE_OK)
            return 1;
    } else if (src.is_package &&
               cli_apply_package(engine, src.manifest_path) != POCKETPARSE_OK) {
        return 1;
    }
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--param") == 0 && i + 1 < argc)
            cli_set_param(engine, argv[++i]);

    /* Embedded grammars are always .scm source; a filesystem grammar may be a
    ** packed .ppg, so it goes through load_grammar (which sniffs the format). */
    if (src.embedded != NULL) {
        pocketparse_source embedded_source;
        embedded_source.name          = cli_text(src.embedded->name);
        embedded_source.bytes.pointer = src.embedded->grammar_bytes;
        embedded_source.bytes.length  = (uint32_t)src.embedded->grammar_length;
        result = pocketparse_compile_scheme(engine, embedded_source, &grammar);
        if (result != POCKETPARSE_OK) {
            pocketparse_string fault = pocketparse_fault(engine);
            fprintf(stderr, "error: %.*s\n", (int)fault.length,
                    (const char *)fault.pointer);
            return 1;
        }
    } else {
        result = load_grammar(engine, src.grammar_path, grammar_bytes,
                              grammar_length, &grammar);
        if (result != POCKETPARSE_OK)
            return 1;
    }

    result = pocketparse_grammar_serialize(grammar, &packed);
    if (result != POCKETPARSE_OK) {
        fprintf(stderr, "pocketparse: error: cannot serialize grammar\n");
        return 1;
    }

    out = fopen(out_path, "wb");
    if (out == NULL) {
        fprintf(stderr, "pocketparse: error: cannot write '%s'\n", out_path);
        return 1;
    }
    if (fwrite(packed.pointer, 1, packed.length, out) != packed.length) {
        fprintf(stderr, "pocketparse: error: short write to '%s'\n", out_path);
        fclose(out);
        return 1;
    }
    fclose(out);
    return 0;
}

/* ** c <grammar.scm> --out <file.c> [--bundle]: compile the .scm, then have the
** freestanding emitter build C source for it (module mode by default, a
** standalone parser program with --bundle), and write the bytes to the output
** file. The library does all the text generation into its arena; this hosted
** shell only handles argv and the file IO. */
static int command_emit(int argc, char **argv)
{
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar = NULL;
    pocketparse_source   source;
    pocketparse_result   result;
    pocketparse_string   emitted;
    char  *grammar_bytes;
    size_t grammar_length;
    const char *out_path = NULL;
    const char *parser_prefix = NULL;
    uint32_t flags = 0;
    cli_grammar_source src;
    FILE  *out;
    int    i;
    /* Library-private W16-3 hook (defined in engine.c, NOT in the public header):
    ** set the compiled-parser symbol prefix for the next emit. Declared here the
    ** way pp_engine_select_vm is, so the public API stays frozen while the CLI can
    ** pass `--name'. */
    extern void pp_engine_set_parser_prefix(pocketparse_engine *engine,
                                            const char *prefix);

    if (argc < 1) {
        fprintf(stderr, "pocketparse: error: c needs a grammar path\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out_path = argv[++i];
        else if (strcmp(argv[i], "--bundle") == 0)
            flags |= POCKETPARSE_EMIT_BUNDLE;
        else if (strcmp(argv[i], "--parser") == 0)
            flags |= POCKETPARSE_EMIT_PARSER;
        else if (strcmp(argv[i], "--parser-lib") == 0)
            flags |= POCKETPARSE_EMIT_PARSER | POCKETPARSE_EMIT_PARSER_LIB;
        /* W16-3: --name P prefixes every emitted file-scope symbol with `P_', so
        ** two parsers generated with distinct prefixes link into one program with
        ** no symbol collision (the embedding use case). With no --name the parser
        ** keeps its historical default symbol names, so the cc-diff oracle and
        ** every existing caller are byte-for-byte unaffected. Only meaningful with
        ** --parser/--parser-lib. */
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
            parser_prefix = argv[++i];
    }
    if (out_path == NULL) {
        fprintf(stderr, "pocketparse: error: c needs --out\n");
        return 1;
    }

    /* Resolve the argument the same way `check'/`run'/`node-types' do, so a
    ** filesystem package, a plain grammar file, or an embedded name all work --
    ** and so a grammar that `extends' a base (std.typescript, std.tsx) resolves
    ** through the installed module resolver. */
    cli_resolve_grammar_source(argv[0], &src);
    if (src.embedded != NULL) {
        source.name          = cli_text(src.embedded->name);
        source.bytes.pointer = src.embedded->grammar_bytes;
        source.bytes.length  = (uint32_t)src.embedded->grammar_length;
    } else {
        grammar_bytes = read_all(src.grammar_path, &grammar_length);
        if (grammar_bytes == NULL) {
            fprintf(stderr, "pocketparse: error: cannot read grammar '%s'\n",
                    src.grammar_path);
            return 1;
        }
        source.name          = cli_text(src.grammar_path);
        source.bytes.pointer = (const uint8_t *)grammar_bytes;
        source.bytes.length  = (uint32_t)grammar_length;
    }

    engine = open_engine();
    if (engine == NULL) {
        fprintf(stderr, "pocketparse: error: out of memory\n");
        return 1;
    }

    cli_install_builtin_classes(engine);
    cli_install_module_resolver(engine, src.grammar_path);
    if (src.embedded != NULL) {
        if (cli_apply_embedded(engine, src.embedded) != POCKETPARSE_OK)
            return 1;
    } else if (src.is_package &&
               cli_apply_package(engine, src.manifest_path) != POCKETPARSE_OK) {
        return 1;
    }
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--param") == 0 && i + 1 < argc)
            cli_set_param(engine, argv[++i]);

    result = pocketparse_compile_scheme(engine, source, &grammar);
    if (result != POCKETPARSE_OK) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "error: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        return 1;
    }

    /* Apply the symbol prefix (if any) just before emit; NULL leaves the default
    ** names in place. */
    pp_engine_set_parser_prefix(engine, parser_prefix);

    result = pocketparse_emit_c(engine, grammar, flags, &emitted);
    if (result != POCKETPARSE_OK) {
        fprintf(stderr, "pocketparse: error: cannot emit C source\n");
        return 1;
    }

    out = fopen(out_path, "wb");
    if (out == NULL) {
        fprintf(stderr, "pocketparse: error: cannot write '%s'\n", out_path);
        return 1;
    }
    if (fwrite(emitted.pointer, 1, emitted.length, out) != emitted.length) {
        fprintf(stderr, "pocketparse: error: short write to '%s'\n", out_path);
        fclose(out);
        return 1;
    }
    fclose(out);
    return 0;
}

/* ** A source line index: the byte offset at which each line begins (line 0 at
** byte 0, line k+1 just past the k-th '\n'). Built once over the source so a node
** byte offset converts to a (line, column) in O(log lines) rather than rescanning
** the source per node. Line and column are both 0-based and column is a byte count
** within the line, matching tree-sitter's [row, column] convention. */
typedef struct dump_line_index {
    uint32_t *starts;   /* starts[k] = byte offset of line k                  */
    uint32_t  count;    /* number of lines                                    */
    const uint8_t *src;
    uint32_t  src_len;
} dump_line_index;

/* ** Build the line index over `src'. On allocation failure `starts' is left NULL
** and the converter falls back to a linear scan, so the dump still works. */
static void dump_line_index_build(dump_line_index *index,
                                  const uint8_t *src, uint32_t src_len)
{
    uint32_t i, lines = 1;
    index->src = src;
    index->src_len = src_len;
    index->starts = NULL;
    index->count = 0;
    for (i = 0; i < src_len; i++)
        if (src[i] == '\n')
            lines++;
    index->starts = (uint32_t *)malloc(lines * sizeof *index->starts);
    if (index->starts == NULL)
        return;
    index->starts[0] = 0;
    index->count = 1;
    for (i = 0; i < src_len; i++)
        if (src[i] == '\n')
            index->starts[index->count++] = i + 1;
}

/* ** Release the line index. */
static void dump_line_index_free(dump_line_index *index)
{
    free(index->starts);
    index->starts = NULL;
}

/* ** Convert a byte offset to a 0-based (line, column), column being the byte
** offset within the line. Binary-searches the line-starts table; falls back to a
** linear scan if the table could not be built. */
static void dump_line_col(const dump_line_index *index, uint32_t byte,
                          uint32_t *out_line, uint32_t *out_col)
{
    if (byte > index->src_len)
        byte = index->src_len;
    if (index->starts == NULL) {
        uint32_t i, line = 0, line_start = 0;
        for (i = 0; i < byte; i++)
            if (index->src[i] == '\n') { line++; line_start = i + 1; }
        *out_line = line;
        *out_col  = byte - line_start;
        return;
    }
    {
        uint32_t lo = 0, hi = index->count - 1, line = 0;
        while (lo <= hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (index->starts[mid] <= byte) { line = mid; lo = mid + 1; }
            else { if (mid == 0) break; hi = mid - 1; }
        }
        *out_line = line;
        *out_col  = byte - index->starts[line];
    }
}

/* ** Print a node's span: byte range and 0-based [line, column] range, in the
** tree-sitter-style `[s-e] [l,c]-[l,c]' shape. Shared by both dumpers so the VM
** and generated parser print identical spans. */
static void dump_span(const dump_line_index *index,
                      uint32_t start_byte, uint32_t end_byte)
{
    uint32_t sl, sc, el, ec;
    dump_line_col(index, start_byte, &sl, &sc);
    dump_line_col(index, end_byte, &el, &ec);
    printf(" [%u-%u] [%u,%u]-[%u,%u]",
           start_byte, end_byte, sl, sc, el, ec);
}

/* ** Recursively dump the subtree under the cursor in pre-order, source order,
** one node per line, indented by depth. A node prints its kind name; a node that
** also carries a field name prints "field: kind", so both the field role and the
** kind are visible (the field name alone hid named kinds reached through a field,
** e.g. an extension's overridden Item or a gated Number). Each line ends with the
** node's span -- byte range and 0-based [line,column] range. Trivia nodes
** (comments) appear in place. */
static void dump_subtree(const pocketparse_tree *tree,
                         pocketparse_tree_cursor *cursor,
                         const dump_line_index *index)
{
    uint32_t depth = pocketparse_tree_cursor_depth(cursor);
    uint32_t i;
    pocketparse_symbol kind  = pocketparse_tree_cursor_kind(cursor);
    pocketparse_symbol field = pocketparse_tree_cursor_field(cursor);
    uint32_t flags = pocketparse_tree_cursor_flags(cursor);
    pocketparse_string kind_name = pocketparse_tree_symbol_name(tree, kind);

    for (i = 0; i < depth; i++)
        fputs("  ", stdout);
    /* A recovery-inserted node is zero-width and absent from the source; mark it
    ** "MISSING <name>" so the dump shows the parser supplied it. */
    if (flags & POCKETPARSE_NODE_MISSING)
        fputs("MISSING ", stdout);
    if (field.id != POCKETPARSE_NO_FIELD) {
        pocketparse_string field_name =
            pocketparse_tree_symbol_name(tree, field);
        printf("%.*s: ", (int)field_name.length,
               (const char *)field_name.pointer);
    }
    printf("%.*s", (int)kind_name.length, (const char *)kind_name.pointer);
    /* W62: a node parsed by an embedded language carries a language tag; print it
    ** as a trailing `[lang]' so the composite tree shows which grammar parsed each
    ** subtree (a JS subtree under a js fence, JSON under a json fence). Host-grammar
    ** nodes are untagged and print nothing extra. */
    {
        uint32_t lang_id = pp_tree_cursor_language_tag(cursor);
        if (lang_id != CLI_NO_LANGUAGE_TAG) {
            pocketparse_symbol lang_symbol;
            pocketparse_string lang_name;
            lang_symbol.id = lang_id;
            lang_name = pocketparse_tree_symbol_name(tree, lang_symbol);
            if (lang_name.length > 0)
                printf(" [%.*s]", (int)lang_name.length,
                       (const char *)lang_name.pointer);
        }
    }
    dump_span(index,
              pocketparse_tree_cursor_start_byte(cursor),
              pocketparse_tree_cursor_end_byte(cursor));
    printf("\n");

    if (pocketparse_tree_cursor_goto_first_child(cursor)) {
        do {
            dump_subtree(tree, cursor, index);
        } while (pocketparse_tree_cursor_goto_next_sibling(cursor));
        pocketparse_tree_cursor_goto_parent(cursor);
    }
}

/* ** The trace plug's emit callback (installed by `run --trace`). It prints one
** line per engine message to stderr as `trace: <name>`, appending ` <data>` when
** the message carries data (the rule or token name). Tracing is observational, so
** it always reports success. This is the hosted side of the plug boundary; the
** library only ever calls this callback, never IO of its own. */
static pocketparse_result cli_trace_emit(void *user,
                                         const pocketparse_message *message)
{
    (void)user;
    fprintf(stderr, "trace: %.*s", (int)message->name.length,
            (const char *)message->name.pointer);
    if (message->data.length > 0)
        fprintf(stderr, " %.*s", (int)message->data.length,
                (const char *)message->data.pointer);
    fputc('\n', stderr);
    return POCKETPARSE_OK;
}

/* ** Install the trace plug under the name "trace" so the engine's trace points
** route their messages to cli_trace_emit. Called by `run` only when --trace is
** given; without it no plug is installed and the engine's trace seam is a no-op. */
static void cli_install_trace_plug(pocketparse_engine *engine)
{
    pocketparse_plug plug;
    plug.name = cli_text("trace");
    plug.emit = cli_trace_emit;
    plug.user = NULL;
    pocketparse_plug_install(engine, &plug);
}

/* ** run <grammar> --input <file|-> [--dump-tree] [--chunk N] [--trace]. */
static int command_run(int argc, char **argv)
{
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar = NULL;
    pocketparse_parser  *parser  = NULL;
    pocketparse_result   result;
    char  *grammar_bytes = NULL, *input_bytes = NULL;
    size_t grammar_length = 0, input_length = 0;
    const char *input_path = NULL;
    cli_grammar_source src;
    int    dump_tree = 0;
    int    trace = 0;
    long   chunk = 0;
    int    i;

    if (argc < 1) {
        fprintf(stderr, "pocketparse: error: run needs a grammar path\n");
        return 1;
    }
    /* Resolve the argument: a filesystem package or file wins; failing both, the
    ** embedded registry is consulted (a path always SHADOWS the embedded copy).
    ** A package's params: are applied after the engine opens, below. */
    cli_resolve_grammar_source(argv[0], &src);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (strcmp(argv[i], "--dump-tree") == 0) {
            dump_tree = 1;
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = 1;
        } else if (strcmp(argv[i], "--chunk") == 0 && i + 1 < argc) {
            chunk = strtol(argv[++i], NULL, 10);
        }
        /* --param NAME[=VALUE] is applied after the engine opens, below. */
    }
    if (input_path == NULL) {
        fprintf(stderr, "pocketparse: error: run needs --input\n");
        return 1;
    }

    if (src.embedded == NULL) {
        grammar_bytes = read_all(src.grammar_path, &grammar_length);
        if (grammar_bytes == NULL) {
            fprintf(stderr, "pocketparse: error: cannot read grammar '%s'\n",
                    src.grammar_path);
            return 1;
        }
    }
    input_bytes = read_all(input_path, &input_length);
    if (input_bytes == NULL) {
        fprintf(stderr, "pocketparse: error: cannot read input '%s'\n",
                input_path);
        return 1;
    }

    engine = open_engine();
    if (engine == NULL) {
        fprintf(stderr, "pocketparse: error: out of memory\n");
        return 1;
    }

    if (trace)
        cli_install_trace_plug(engine);
    cli_install_builtin_classes(engine);
    cli_install_module_resolver(engine, src.grammar_path);
    /* Bind the manifest's params: first, then let a --param override them. */
    if (src.embedded != NULL) {
        if (cli_apply_embedded(engine, src.embedded) != POCKETPARSE_OK)
            return 1;
    } else if (src.is_package &&
               cli_apply_package(engine, src.manifest_path) != POCKETPARSE_OK) {
        return 1;
    }
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--param") == 0 && i + 1 < argc)
            cli_set_param(engine, argv[++i]);
    /* Embedded grammars are always .scm source; compile them directly. A
    ** filesystem grammar may be a packed .ppg, so it goes through load_grammar. */
    if (src.embedded != NULL) {
        pocketparse_source embedded_source;
        embedded_source.name          = cli_text(src.embedded->name);
        embedded_source.bytes.pointer = src.embedded->grammar_bytes;
        embedded_source.bytes.length  = (uint32_t)src.embedded->grammar_length;
        result = pocketparse_compile_scheme(engine, embedded_source, &grammar);
        if (result != POCKETPARSE_OK) {
            pocketparse_string fault = pocketparse_fault(engine);
            fprintf(stderr, "error: %.*s\n", (int)fault.length,
                    (const char *)fault.pointer);
            return 1;
        }
    } else {
        result = load_grammar(engine, src.grammar_path, grammar_bytes,
                              grammar_length, &grammar);
        if (result != POCKETPARSE_OK)
            return 1;
    }

    {
        pocketparse_string empty;
        empty.pointer = NULL;
        empty.length  = 0;
        result = pocketparse_parser_new(engine, grammar, empty, &parser);
    }
    if (result != POCKETPARSE_OK || parser == NULL) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "error: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        return 1;
    }

    /* Push the input, optionally split into N-byte chunks to exercise the
    ** streaming buffer one piece at a time. */
    {
        size_t step = (chunk > 0) ? (size_t)chunk : input_length;
        size_t offset = 0;
        if (step == 0)
            step = 1;
        if (input_length == 0) {
            pocketparse_string nothing;
            nothing.pointer = (const uint8_t *)input_bytes;
            nothing.length  = 0;
            pocketparse_push(parser, nothing);
        }
        while (offset < input_length) {
            pocketparse_string chunk_bytes;
            size_t n = input_length - offset;
            if (n > step)
                n = step;
            chunk_bytes.pointer = (const uint8_t *)(input_bytes + offset);
            chunk_bytes.length  = (uint32_t)n;
            pocketparse_push(parser, chunk_bytes);
            offset += n;
        }
    }

    result = pocketparse_finish(parser);

    /* W60 structural proof: when asked, report whether the parse genuinely ran
    ** inside the ax actor (the parser actor's input.end action set this flag after
    ** running the dispatch loop). This is the inverse of the Phase 11 gap -- ax
    ** used only as an arena. It costs nothing unless POCKETPARSE_TRACE_ACTOR is set. */
    {
        const char *trace_actor = getenv("POCKETPARSE_TRACE_ACTOR");
        if (trace_actor != NULL && trace_actor[0] != '\0')
            fprintf(stderr, "actor: parse ran on ax = %d\n",
                    pp_parser_actor_ran(parser));
    }

    /* A hard syntax error yields no usable tree: report the diagnostics and
    ** stop. Recovery is different -- the parse completes and builds a tree, but
    ** still records error-severity diagnostics; there the tree is dumped (so the
    ** MISSING nodes show) and the diagnostics are reported, and the exit code is
    ** nonzero whenever any error-severity diagnostic was produced. */
    if (result == POCKETPARSE_SYNTAX_ERROR || result == POCKETPARSE_ERROR) {
        uint32_t count = pocketparse_parser_diagnostic_count(parser);
        uint32_t d;
        if (count == 0) {
            fprintf(stderr, "error: parse failed\n");
        }
        for (d = 0; d < count; d++) {
            const pocketparse_diagnostic *diag =
                pocketparse_parser_diagnostic(parser, d);
            if (diag != NULL)
                render_diagnostic(diag);
        }
        return 1;
    }

    if (dump_tree) {
        const pocketparse_tree *tree = pocketparse_parser_tree(parser);
        pocketparse_tree_cursor *cursor = NULL;
        if (tree != NULL &&
            pocketparse_tree_cursor_new(tree, &cursor) == POCKETPARSE_OK) {
            dump_line_index index;
            dump_line_index_build(&index, (const uint8_t *)input_bytes,
                                  (uint32_t)input_length);
            dump_subtree(tree, cursor, &index);
            dump_line_index_free(&index);
            pocketparse_tree_cursor_free(cursor);
        }
    }

    /* Report any diagnostics gathered during a successful (recovered) parse and
    ** fail if any reached error severity. */
    {
        uint32_t count = pocketparse_parser_diagnostic_count(parser);
        uint32_t d;
        int had_error = 0;
        for (d = 0; d < count; d++) {
            const pocketparse_diagnostic *diag =
                pocketparse_parser_diagnostic(parser, d);
            if (diag == NULL)
                continue;
            render_diagnostic(diag);
            if (diag->severity == POCKETPARSE_SEVERITY_ERROR)
                had_error = 1;
        }
        if (had_error)
            return 1;
    }
    return 0;
}

/* ** Resolve a grammar argument, parse input bytes against it, and hand back the
** opened engine and the finished parser (whose tree the caller queries). This is
** the shared front half of the `linearize' and `query' commands, mirroring the
** setup in command_run minus the tree dump: resolve the grammar (a filesystem
** path SHADOWS the embedded registry), apply a package's params:, --param
** overrides, compile or load the grammar, then push the whole input and finish.
** argv/argc are the command's own arguments; arg0 names the grammar, and the
** caller has already located the --input bytes. Returns 0 on success, writing
** *out_engine and *out_parser; nonzero on any failure (after printing an error).
** A hard syntax error is a failure here -- there is no tree to walk. */
static int cli_parse_input(const char *grammar_arg, int argc, char **argv,
                           const char *input_bytes, size_t input_length,
                           pocketparse_engine  **out_engine,
                           pocketparse_parser  **out_parser)
{
    pocketparse_engine  *engine;
    pocketparse_grammar *grammar = NULL;
    pocketparse_parser  *parser  = NULL;
    pocketparse_result   result;
    char  *grammar_bytes = NULL;
    size_t grammar_length = 0;
    cli_grammar_source src;
    int    i;

    *out_engine = NULL;
    *out_parser = NULL;
    cli_resolve_grammar_source(grammar_arg, &src);

    if (src.embedded == NULL) {
        grammar_bytes = read_all(src.grammar_path, &grammar_length);
        if (grammar_bytes == NULL) {
            fprintf(stderr, "pocketparse: error: cannot read grammar '%s'\n",
                    src.grammar_path);
            return 1;
        }
    }

    engine = open_engine();
    if (engine == NULL) {
        fprintf(stderr, "pocketparse: error: out of memory\n");
        return 1;
    }
    cli_install_builtin_classes(engine);
    cli_install_module_resolver(engine, src.grammar_path);
    if (src.embedded != NULL) {
        if (cli_apply_embedded(engine, src.embedded) != POCKETPARSE_OK)
            return 1;
    } else if (src.is_package &&
               cli_apply_package(engine, src.manifest_path) != POCKETPARSE_OK) {
        return 1;
    }
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--param") == 0 && i + 1 < argc)
            cli_set_param(engine, argv[++i]);

    if (src.embedded != NULL) {
        pocketparse_source embedded_source;
        embedded_source.name          = cli_text(src.embedded->name);
        embedded_source.bytes.pointer = src.embedded->grammar_bytes;
        embedded_source.bytes.length  = (uint32_t)src.embedded->grammar_length;
        result = pocketparse_compile_scheme(engine, embedded_source, &grammar);
        if (result != POCKETPARSE_OK) {
            pocketparse_string fault = pocketparse_fault(engine);
            fprintf(stderr, "error: %.*s\n", (int)fault.length,
                    (const char *)fault.pointer);
            return 1;
        }
    } else {
        result = load_grammar(engine, src.grammar_path, grammar_bytes,
                              grammar_length, &grammar);
        if (result != POCKETPARSE_OK)
            return 1;
    }

    {
        pocketparse_string empty;
        empty.pointer = NULL;
        empty.length  = 0;
        result = pocketparse_parser_new(engine, grammar, empty, &parser);
    }
    if (result != POCKETPARSE_OK || parser == NULL) {
        pocketparse_string fault = pocketparse_fault(engine);
        fprintf(stderr, "error: %.*s\n", (int)fault.length,
                (const char *)fault.pointer);
        return 1;
    }

    {
        pocketparse_string bytes;
        bytes.pointer = (const uint8_t *)input_bytes;
        bytes.length  = (uint32_t)input_length;
        pocketparse_push(parser, bytes);
    }
    result = pocketparse_finish(parser);
    if (result == POCKETPARSE_SYNTAX_ERROR || result == POCKETPARSE_ERROR) {
        uint32_t count = pocketparse_parser_diagnostic_count(parser);
        uint32_t d;
        if (count == 0)
            fprintf(stderr, "error: parse failed\n");
        for (d = 0; d < count; d++) {
            const pocketparse_diagnostic *diag =
                pocketparse_parser_diagnostic(parser, d);
            if (diag != NULL)
                render_diagnostic(diag);
        }
        return 1;
    }

    *out_engine = engine;
    *out_parser = parser;
    return 0;
}

/* ** The linearize command's emit callback: print one ENTER/LEAF/EXIT token per
** line as `enter <Kind>` / `leaf <Kind> <text>` / `exit <Kind>`, the debugging
** view a query parses. Only a LEAF carries text. */
static void cli_linearize_emit(void *user, const pocketparse_lin_token *token)
{
    (void)user;
    switch (token->kind) {
    case POCKETPARSE_LIN_ENTER:
        printf("enter %.*s\n", (int)token->node_kind.length,
               (const char *)token->node_kind.pointer);
        break;
    case POCKETPARSE_LIN_LEAF:
        printf("leaf %.*s %.*s\n", (int)token->node_kind.length,
               (const char *)token->node_kind.pointer,
               (int)token->text.length, (const char *)token->text.pointer);
        break;
    case POCKETPARSE_LIN_EXIT:
        printf("exit %.*s\n", (int)token->node_kind.length,
               (const char *)token->node_kind.pointer);
        break;
    }
}

/* ** linearize <grammar> --input <file|->: parse the input and print its tree's
** ENTER/LEAF/EXIT linearization, the stream a query parses. A debugging view. */
static int command_linearize(int argc, char **argv)
{
    pocketparse_engine *engine = NULL;
    pocketparse_parser *parser = NULL;
    const pocketparse_tree *tree;
    const char *input_path = NULL;
    char  *input_bytes = NULL;
    size_t input_length = 0;
    int    i;

    if (argc < 1) {
        fprintf(stderr, "pocketparse: error: linearize needs a grammar path\n");
        return 1;
    }
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            input_path = argv[++i];
    if (input_path == NULL) {
        fprintf(stderr, "pocketparse: error: linearize needs --input\n");
        return 1;
    }
    input_bytes = read_all(input_path, &input_length);
    if (input_bytes == NULL) {
        fprintf(stderr, "pocketparse: error: cannot read input '%s'\n",
                input_path);
        return 1;
    }
    if (cli_parse_input(argv[0], argc, argv, input_bytes, input_length,
                        &engine, &parser) != 0)
        return 1;

    tree = pocketparse_parser_tree(parser);
    {
        pocketparse_string input;
        input.pointer = (const uint8_t *)input_bytes;
        input.length  = (uint32_t)input_length;
        pocketparse_linearize(engine, tree, input, cli_linearize_emit, NULL);
    }
    return 0;
}

/* ** The query command's emit callback: print one line per capture as
** `@<name> <start>-<end> <text>'. */
static void cli_query_emit(void *user, const pocketparse_query_capture *capture)
{
    (void)user;
    printf("@%.*s %u-%u %.*s\n", (int)capture->name.length,
           (const char *)capture->name.pointer,
           capture->start_byte, capture->end_byte,
           (int)capture->text.length, (const char *)capture->text.pointer);
}

/* ** query <grammar> <query.scm> --input <file|->: parse the input against the
** grammar, run the query over the resulting tree, and print one line per capture
** as `@<name> <start>-<end> <text>'. The grammar is resolved the same way `run'
** resolves it (a filesystem path SHADOWS the embedded registry). */
static int command_query(int argc, char **argv)
{
    pocketparse_engine *engine = NULL;
    pocketparse_parser *parser = NULL;
    const pocketparse_tree *tree;
    const char *query_path = NULL;
    const char *input_path = NULL;
    char  *query_bytes = NULL, *input_bytes = NULL;
    size_t query_length = 0, input_length = 0;
    int    i;
    pocketparse_result result;

    if (argc < 2) {
        fprintf(stderr,
                "pocketparse: error: query needs a grammar and a query path\n");
        return 1;
    }
    query_path = argv[1];
    for (i = 2; i < argc; i++)
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            input_path = argv[++i];
    if (input_path == NULL) {
        fprintf(stderr, "pocketparse: error: query needs --input\n");
        return 1;
    }
    query_bytes = read_all(query_path, &query_length);
    if (query_bytes == NULL) {
        fprintf(stderr, "pocketparse: error: cannot read query '%s'\n",
                query_path);
        return 1;
    }
    input_bytes = read_all(input_path, &input_length);
    if (input_bytes == NULL) {
        fprintf(stderr, "pocketparse: error: cannot read input '%s'\n",
                input_path);
        return 1;
    }
    /* The query path is positional; skip it when scanning for --param so it is
    ** not mistaken for one. cli_parse_input scans from index 1, and argv[1] is
    ** the query path, never a flag, so it is harmless to leave in place. */
    if (cli_parse_input(argv[0], argc, argv, input_bytes, input_length,
                        &engine, &parser) != 0)
        return 1;

    tree = pocketparse_parser_tree(parser);
    {
        pocketparse_string query, input;
        query.pointer = (const uint8_t *)query_bytes;
        query.length  = (uint32_t)query_length;
        input.pointer = (const uint8_t *)input_bytes;
        input.length  = (uint32_t)input_length;
        result = pocketparse_query(engine, tree, query, input,
                                   cli_query_emit, NULL);
    }
    if (result != POCKETPARSE_OK) {
        fprintf(stderr, "pocketparse: error: invalid query\n");
        return 1;
    }
    return 0;
}

/* ** The starter grammar a freshly seeded project carries when no --grammar
** names a standard package: a tiny "hello" grammar. It tokenizes a greeting --
** the word "hello" or "Hello", optional whitespace, and a name made of letters
** -- into a Greeting node with a Name field. It needs no host CLASS bindings
** (it uses regex classes), so the starter main.c can compile it with nothing
** beyond pocketparse_open and pocketparse_compile_scheme. */
static const char pocketparse_starter_grammar[] =
    "; grammar.scm -- a starter \"hello\" grammar for a seeded pocketparse project.\n"
    ";\n"
    "; It recognises a greeting: the word \"hello\" (any case), optional space,\n"
    "; and a name of letters. Whitespace between tokens is trivia (extras), so it\n"
    "; is skipped. Edit this file and rebuild -- main.c compiles it at runtime.\n"
    "\n"
    "(pocketparse version: \"0.1\")\n"
    "\n"
    "(grammar hello\n"
    "  start: Greeting\n"
    "\n"
    "  (token Space (regex \"[ \\t\\r\\n]+\"))\n"
    "  (token Hello (regex \"[Hh]ello\"))\n"
    "  (token Word  (regex \"[A-Za-z]+\"))\n"
    "\n"
    "  (rule Greeting\n"
    "    (seq Hello (field name: Word) EOF))\n"
    "\n"
    "  (extras\n"
    "    (trivia Space)))\n";

/* ** The starter main.c a freshly seeded project carries. It is the on-ramp the
** design promises: open an engine over a stack grant, read the sibling
** grammar.scm, compile it at runtime through pocketparse_compile_scheme, parse
** stdin, and dump the resulting tree through the public cursor API. It depends
** on nothing but pocketparse.h, so once seeded the project needs no pocketparse
** tooling on the path. Built with `cc main.c pocketparse.c -o app`. */
static const char pocketparse_starter_main[] =
    "/*\n"
    " * main.c -- a starter pocketparse program.\n"
    " *\n"
    " * It opens an engine, compiles the sibling grammar.scm at runtime, parses\n"
    " * standard input against it, and prints the concrete syntax tree. The whole\n"
    " * engine is in pocketparse.c (the ax actor system is folded in and hidden),\n"
    " * so a single C compiler builds the project with no other dependency:\n"
    " *\n"
    " *     cc main.c pocketparse.c -o app\n"
    " *     echo 'hello world' | ./app\n"
    " */\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "#include \"pocketparse.h\"\n"
    "\n"
    "/* Read an entire stream into a malloc'd buffer; writes the length. */\n"
    "static char *read_all(FILE *file, size_t *out_length)\n"
    "{\n"
    "    char  *buffer = NULL;\n"
    "    size_t length = 0, capacity = 0;\n"
    "    for (;;) {\n"
    "        size_t got;\n"
    "        if (length + 65536 > capacity) {\n"
    "            size_t nc = capacity == 0 ? 65536 : capacity * 2;\n"
    "            char *grown = realloc(buffer, nc);\n"
    "            if (grown == NULL) { free(buffer); return NULL; }\n"
    "            buffer = grown; capacity = nc;\n"
    "        }\n"
    "        got = fread(buffer + length, 1, 65536, file);\n"
    "        length += got;\n"
    "        if (got < 65536) break;\n"
    "    }\n"
    "    *out_length = length;\n"
    "    return buffer;\n"
    "}\n"
    "\n"
    "/* Print the subtree under the cursor, one node per line, indented by depth. */\n"
    "static void dump(const pocketparse_tree *tree, pocketparse_tree_cursor *cursor)\n"
    "{\n"
    "    uint32_t depth = pocketparse_tree_cursor_depth(cursor), i;\n"
    "    pocketparse_symbol kind = pocketparse_tree_cursor_kind(cursor);\n"
    "    pocketparse_string name = pocketparse_tree_symbol_name(tree, kind);\n"
    "    for (i = 0; i < depth; i++) fputs(\"  \", stdout);\n"
    "    printf(\"%.*s\\n\", (int)name.length, (const char *)name.pointer);\n"
    "    if (pocketparse_tree_cursor_goto_first_child(cursor)) {\n"
    "        do { dump(tree, cursor); }\n"
    "        while (pocketparse_tree_cursor_goto_next_sibling(cursor));\n"
    "        pocketparse_tree_cursor_goto_parent(cursor);\n"
    "    }\n"
    "}\n"
    "\n"
    "int main(void)\n"
    "{\n"
    "    static unsigned char grant[8u * 1024u * 1024u];\n"
    "    pocketparse_configuration cfg;\n"
    "    pocketparse_engine  *engine;\n"
    "    pocketparse_grammar *grammar = NULL;\n"
    "    pocketparse_parser  *parser  = NULL;\n"
    "    pocketparse_source   source;\n"
    "    pocketparse_string   empty;\n"
    "    char  *grammar_bytes, *input_bytes;\n"
    "    size_t grammar_length = 0, input_length = 0;\n"
    "    FILE  *gf;\n"
    "\n"
    "    /* Open an engine over a fixed grant. The library never allocates outside\n"
    "     * it (and grows by spilling, which we do not need here). */\n"
    "    memset(&cfg, 0, sizeof cfg);\n"
    "    cfg.memory.base = grant;\n"
    "    cfg.memory.size = sizeof grant;\n"
    "    engine = pocketparse_open(cfg);\n"
    "    if (engine == NULL) { fprintf(stderr, \"error: cannot open engine\\n\"); return 1; }\n"
    "\n"
    "    /* Read and compile the sibling grammar.scm at runtime. */\n"
    "    gf = fopen(\"grammar.scm\", \"rb\");\n"
    "    if (gf == NULL) { fprintf(stderr, \"error: cannot open grammar.scm\\n\"); return 1; }\n"
    "    grammar_bytes = read_all(gf, &grammar_length);\n"
    "    fclose(gf);\n"
    "    if (grammar_bytes == NULL) { fprintf(stderr, \"error: out of memory\\n\"); return 1; }\n"
    "    source.name.pointer = (const uint8_t *)\"grammar.scm\";\n"
    "    source.name.length  = (uint32_t)strlen(\"grammar.scm\");\n"
    "    source.bytes.pointer = (const uint8_t *)grammar_bytes;\n"
    "    source.bytes.length  = (uint32_t)grammar_length;\n"
    "    if (pocketparse_compile_scheme(engine, source, &grammar) != POCKETPARSE_OK) {\n"
    "        pocketparse_string f = pocketparse_fault(engine);\n"
    "        fprintf(stderr, \"error: %.*s\\n\", (int)f.length, (const char *)f.pointer);\n"
    "        return 1;\n"
    "    }\n"
    "\n"
    "    /* Parse standard input against the grammar. */\n"
    "    empty.pointer = NULL; empty.length = 0;\n"
    "    if (pocketparse_parser_new(engine, grammar, empty, &parser) != POCKETPARSE_OK) {\n"
    "        fprintf(stderr, \"error: cannot create parser\\n\"); return 1;\n"
    "    }\n"
    "    input_bytes = read_all(stdin, &input_length);\n"
    "    if (input_bytes == NULL) { fprintf(stderr, \"error: out of memory\\n\"); return 1; }\n"
    "    {\n"
    "        pocketparse_string chunk;\n"
    "        chunk.pointer = (const uint8_t *)input_bytes;\n"
    "        chunk.length  = (uint32_t)input_length;\n"
    "        pocketparse_push(parser, chunk);\n"
    "    }\n"
    "    if (pocketparse_finish(parser) != POCKETPARSE_OK) {\n"
    "        uint32_t n = pocketparse_parser_diagnostic_count(parser), d;\n"
    "        for (d = 0; d < n; d++) {\n"
    "            const pocketparse_diagnostic *diag = pocketparse_parser_diagnostic(parser, d);\n"
    "            if (diag != NULL)\n"
    "                fprintf(stderr, \"%.*s\\n\", (int)diag->message.length,\n"
    "                        (const char *)diag->message.pointer);\n"
    "        }\n"
    "        return 1;\n"
    "    }\n"
    "\n"
    "    /* Dump the tree. */\n"
    "    {\n"
    "        const pocketparse_tree *tree = pocketparse_parser_tree(parser);\n"
    "        pocketparse_tree_cursor *cursor = NULL;\n"
    "        if (tree != NULL &&\n"
    "            pocketparse_tree_cursor_new(tree, &cursor) == POCKETPARSE_OK) {\n"
    "            dump(tree, cursor);\n"
    "            pocketparse_tree_cursor_free(cursor);\n"
    "        }\n"
    "    }\n"
    "    return 0;\n"
    "}\n";

/* ** The starter build.sh a seeded amalgamation project carries: the one-line
** build the design promises. */
static const char pocketparse_starter_build[] =
    "#!/bin/sh\n"
    "# Build the seeded project. The whole engine is in pocketparse.c, so one C\n"
    "# compiler and no other dependency suffices.\n"
    "cc main.c pocketparse.c -o app\n";

/* ** The starter build.sh for --lib mode: link against an installed
** libpocketparse instead of compiling the vendored engine. */
static const char pocketparse_starter_build_lib[] =
    "#!/bin/sh\n"
    "# Build the seeded project against an installed libpocketparse.\n"
    "cc main.c -lpocketparse -o app\n";

/* ** Create a directory (and report the failure to stderr). Succeeds when the
** directory already exists. Returns 1 on success, 0 on failure. */
static int seed_make_dir(const char *path)
{
    if (mkdir(path, 0777) == 0)
        return 1;
    if (errno == EEXIST)
        return 1;
    fprintf(stderr, "pocketparse: error: cannot create directory '%s'\n", path);
    return 0;
}

/* ** Join a directory and a file name into a freshly malloc'd, NUL-terminated
** path, inserting a single '/' unless dir already ends in one or is empty. The
** result has no length cap; returns NULL only on allocation failure. The caller
** frees it. */
static char *seed_join(const char *dir, const char *file)
{
    cli_strbuf  out;
    size_t      len = strlen(dir);
    const char *sep = "/";
    if (len == 0)
        sep = "";
    else if (dir[len - 1] == '/')
        sep = "";
    cli_strbuf_init(&out);
    cli_strbuf_append_cstr(&out, dir);
    cli_strbuf_append_cstr(&out, sep);
    cli_strbuf_append_cstr(&out, file);
    if (out.failed) {
        free(out.data);
        return NULL;
    }
    return out.data ? out.data : cli_strdup("");
}

/* ** Write a byte buffer to dir/file. Reports any failure to stderr and returns
** 1 on success, 0 otherwise. The path is built with seed_join. */
static int seed_write_file(const char *dir, const char *file,
                           const unsigned char *bytes, size_t length)
{
    char *path;
    FILE *out;

    path = seed_join(dir, file);
    if (path == NULL) {
        fprintf(stderr, "pocketparse: error: out of memory for path '%s'\n", file);
        return 0;
    }
    out = fopen(path, "wb");
    if (out == NULL) {
        fprintf(stderr, "pocketparse: error: cannot write '%s'\n", path);
        free(path);
        return 0;
    }
    if (length > 0 && fwrite(bytes, 1, length, out) != length) {
        fprintf(stderr, "pocketparse: error: short write to '%s'\n", path);
        fclose(out);
        free(path);
        return 0;
    }
    fclose(out);
    free(path);
    return 1;
}

/* ** Write the embedded library amalgamation (pocketparse.c + pocketparse.h)
** into dir. Shared by `init --amalgamation' and `vendor --amalgamation'. */
static int seed_write_amalgamation(const char *dir)
{
    if (!seed_write_file(dir, "pocketparse.c",
                         pocketparse_amalgamation_c_bytes,
                         pocketparse_amalgamation_c_length))
        return 0;
    if (!seed_write_file(dir, "pocketparse.h",
                         pocketparse_amalgamation_h_bytes,
                         pocketparse_amalgamation_h_length))
        return 0;
    return 1;
}

/* ** Write the starter grammar.scm into dir. When pkg is non-NULL it is an
** embedded standard package whose grammar bytes are written instead of the
** built-in hello grammar (the --grammar NAME case). */
static int seed_write_grammar(const char *dir,
                              const pocketparse_embedded_package *pkg)
{
    if (pkg != NULL)
        return seed_write_file(dir, "grammar.scm", pkg->grammar_bytes,
                               (size_t)pkg->grammar_length);
    return seed_write_file(dir, "grammar.scm",
                           (const unsigned char *)pocketparse_starter_grammar,
                           sizeof pocketparse_starter_grammar - 1);
}

/* ** A parsed seed command line: the target directory, the mode (amalgamation
** or lib), and the optional --grammar package name. */
typedef struct seed_options {
    const char *dir;
    int         amalgamation; /* 1 = --amalgamation (default), 0 = --lib */
    const char *grammar_name; /* --grammar NAME, or NULL for the hello grammar */
} seed_options;

/* ** Parse the shared init/vendor option grammar from argv. The first
** non-option argument is the directory; --amalgamation (default) and --lib
** select the mode; --grammar NAME names a standard grammar. Returns 1 on a
** well-formed command line, 0 (after printing an error) otherwise. */
static int seed_parse_options(int argc, char **argv, const char *verb,
                              seed_options *out)
{
    int i;
    out->dir          = NULL;
    out->amalgamation = 1;
    out->grammar_name = NULL;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--amalgamation") == 0) {
            out->amalgamation = 1;
        } else if (strcmp(argv[i], "--lib") == 0) {
            out->amalgamation = 0;
        } else if (strcmp(argv[i], "--grammar") == 0 && i + 1 < argc) {
            out->grammar_name = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "pocketparse: error: %s: unknown option '%s'\n",
                    verb, argv[i]);
            return 0;
        } else if (out->dir == NULL) {
            out->dir = argv[i];
        }
    }
    if (out->dir == NULL) {
        fprintf(stderr, "pocketparse: error: %s needs a directory\n", verb);
        return 0;
    }
    return 1;
}

/* ** Resolve a --grammar NAME to an embedded standard package, or fail. Returns
** 1 and writes *out_pkg (NULL when no name was given), 0 after printing an error
** when a name was given that names no embedded package. */
static int seed_resolve_grammar(const seed_options *opts,
                                const pocketparse_embedded_package **out_pkg)
{
    *out_pkg = NULL;
    if (opts->grammar_name == NULL)
        return 1;
    *out_pkg = cli_embedded_lookup(opts->grammar_name);
    if (*out_pkg == NULL) {
        fprintf(stderr, "pocketparse: error: unknown grammar '%s'\n",
                opts->grammar_name);
        return 0;
    }
    return 1;
}

/* ** init <dir> [--amalgamation|--lib] [--grammar NAME]: seed a new project. In
** --amalgamation mode (the default) it drops the embedded engine source
** (pocketparse.c + pocketparse.h), a starter grammar.scm, a main.c that compiles
** that grammar at runtime and dumps the parse of stdin, and a one-line build.sh;
** the seeded project then builds with a single cc and needs no pocketparse
** tooling. In --lib mode it writes the same main.c and grammar.scm but copies no
** engine source, leaving the project to link an installed libpocketparse. */
static int command_init(int argc, char **argv)
{
    seed_options opts;
    const pocketparse_embedded_package *pkg = NULL;

    if (!seed_parse_options(argc, argv, "init", &opts))
        return 1;
    if (!seed_resolve_grammar(&opts, &pkg))
        return 1;
    if (!seed_make_dir(opts.dir))
        return 1;

    if (opts.amalgamation) {
        if (!seed_write_amalgamation(opts.dir))
            return 1;
    }
    if (!seed_write_grammar(opts.dir, pkg))
        return 1;
    if (!seed_write_file(opts.dir, "main.c",
                         (const unsigned char *)pocketparse_starter_main,
                         sizeof pocketparse_starter_main - 1))
        return 1;
    if (opts.amalgamation) {
        if (!seed_write_file(opts.dir, "build.sh",
                             (const unsigned char *)pocketparse_starter_build,
                             sizeof pocketparse_starter_build - 1))
            return 1;
    } else {
        if (!seed_write_file(opts.dir, "build.sh",
                             (const unsigned char *)pocketparse_starter_build_lib,
                             sizeof pocketparse_starter_build_lib - 1))
            return 1;
    }
    return 0;
}

/* ** vendor <dir> [--amalgamation|--lib]: drop or refresh just the engine source
** into an existing project, writing no scaffold. In --amalgamation mode it writes
** pocketparse.c + pocketparse.h; in --lib mode it writes only pocketparse.h (the
** project links an installed libpocketparse for the object code). */
static int command_vendor(int argc, char **argv)
{
    seed_options opts;

    if (!seed_parse_options(argc, argv, "vendor", &opts))
        return 1;
    if (!seed_make_dir(opts.dir))
        return 1;

    if (opts.amalgamation) {
        if (!seed_write_amalgamation(opts.dir))
            return 1;
    } else {
        if (!seed_write_file(opts.dir, "pocketparse.h",
                             pocketparse_amalgamation_h_bytes,
                             pocketparse_amalgamation_h_length))
            return 1;
    }
    return 0;
}

/* ** Dispatch on the first argument. */
int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(stdout);
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "help") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "version") == 0 || strcmp(argv[1], "--version") == 0)
        return command_version();
    if (strcmp(argv[1], "man") == 0)
        return command_man();
    if (strcmp(argv[1], "grammars") == 0)
        return command_grammars();
    if (strcmp(argv[1], "check") == 0)
        return command_check(argc - 2, argv + 2);
    if (strcmp(argv[1], "run") == 0)
        return command_run(argc - 2, argv + 2);
    if (strcmp(argv[1], "pack") == 0)
        return command_pack(argc - 2, argv + 2);
    if (strcmp(argv[1], "c") == 0)
        return command_emit(argc - 2, argv + 2);
    if (strcmp(argv[1], "linearize") == 0)
        return command_linearize(argc - 2, argv + 2);
    if (strcmp(argv[1], "query") == 0)
        return command_query(argc - 2, argv + 2);
    if (strcmp(argv[1], "node-types") == 0)
        return command_node_types(argc - 2, argv + 2);
    if (strcmp(argv[1], "analyze-tailcalls") == 0)
        return command_analyze_tailcalls(argc - 2, argv + 2);
    if (strcmp(argv[1], "init") == 0)
        return command_init(argc - 2, argv + 2);
    if (strcmp(argv[1], "vendor") == 0)
        return command_vendor(argc - 2, argv + 2);

    fprintf(stderr, "pocketparse: error: unknown command '%s'\n", argv[1]);
    print_usage(stderr);
    return 2;
}
