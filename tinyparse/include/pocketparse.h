/*
** pocketparse.h -- the public contract for pocketparse.
**
** pocketparse compiles grammar data into running parsers at runtime. The
** core library is freestanding C: it depends on nothing but <stddef.h> and
** <stdint.h>, never allocates outside a host-granted memory pool, and never
** touches libc, IO, the filesystem, threads, a clock, or global mutable
** state.
**
** This header is the frozen public surface, translated from the design
** document's "Public shape" into regular C99. No symbol here takes a
** null-terminated string: pocketparse_string carries an explicit length and
** is authoritative. Functions whose bodies are not yet implemented still
** appear here -- this file is the contract, not the engine.
*/
#ifndef POCKETPARSE_H
#define POCKETPARSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ** A length-prefixed byte slice. Length is authoritative; a trailing NUL
** may exist but is never required. Used for every string in the API. */
typedef struct pocketparse_string {
    const uint8_t *pointer;
    uint32_t       length;
} pocketparse_string;

/* ** The host's memory grant. pocketparse carves all of its arenas from this
** block and never allocates outside it. grow may be NULL for a fixed budget;
** when present it returns a fresh block (never a realloc) so existing
** allocations keep their addresses. The grant is handed straight to the
** library's arena allocator: grow is asked for at least
** `need' bytes and writes the block's true usable size back through `got' (a
** host that hands back exactly `need' simply leaves *got = need). chunk_minimum
** is the floor on the chunk an arena pulls; zero selects the built-in default. */
typedef struct pocketparse_memory {
    void   *base;
    size_t  size;
    void  *(*grow)(void *user, size_t need, size_t *got);
    void   *user;
    size_t  chunk_minimum;
} pocketparse_memory;

/* ** Open-time configuration: the memory grant plus a flag word reserved for
** future behaviour selectors. */
typedef struct pocketparse_configuration {
    pocketparse_memory memory;
    uint32_t           flags;
} pocketparse_configuration;

/* ** Every public result code. OK/MORE/DONE report progress; the remainder
** report failure without ever crashing. */
typedef enum pocketparse_result {
    POCKETPARSE_OK = 0,
    POCKETPARSE_MORE,
    POCKETPARSE_DONE,
    POCKETPARSE_OUT_OF_MEMORY,
    POCKETPARSE_INVALID,
    POCKETPARSE_SYNTAX_ERROR,
    POCKETPARSE_ERROR
} pocketparse_result;

/* ** The opaque public handles; callers see only ordinary C pointers. */
typedef struct pocketparse_engine  pocketparse_engine;
typedef struct pocketparse_grammar pocketparse_grammar;
typedef struct pocketparse_parser  pocketparse_parser;
typedef struct pocketparse_tree    pocketparse_tree;

/* ** Named input bytes: a source name (for diagnostics) and the bytes
** themselves. Used both for grammar text and for parser input. */
typedef struct pocketparse_source {
    pocketparse_string name;
    pocketparse_string bytes;
} pocketparse_source;

/* ** A derived line/column position. Byte offset is the canonical position;
** points are computed from a line map, not treated as primary. */
typedef struct pocketparse_point {
    uint32_t row;
    uint32_t column;
} pocketparse_point;

/* ** A byte span with its derived start and end points. */
typedef struct pocketparse_range {
    uint32_t          start_byte;
    uint32_t          end_byte;
    pocketparse_point start_point;
    pocketparse_point end_point;
} pocketparse_range;

/* ** Diagnostic severity, in increasing order of seriousness. */
typedef enum pocketparse_severity {
    POCKETPARSE_SEVERITY_NOTE = 0,
    POCKETPARSE_SEVERITY_WARNING,
    POCKETPARSE_SEVERITY_ERROR
} pocketparse_severity;

/* ** One annotated span within a diagnostic: a byte range and a message,
** plus a flag word for rendering hints. */
typedef struct pocketparse_label {
    uint32_t           start_byte;
    uint32_t           end_byte;
    pocketparse_string message;
    uint32_t           flags;
} pocketparse_label;

/* ** A structured diagnostic. The core never prints; it produces this data
** and leaves rendering to the host. A diagnostic carries a severity, a
** short code, a primary message, zero or more labelled spans, and optional
** help and note text. */
typedef struct pocketparse_diagnostic {
    pocketparse_severity     severity;
    pocketparse_string       code;
    pocketparse_string       message;
    const pocketparse_label *labels;
    uint32_t                 label_count;
    pocketparse_string       help;
    pocketparse_string       note;
} pocketparse_diagnostic;

/* ** A symbol identity: an index into the tree's symbol table. */
typedef struct pocketparse_symbol {
    uint32_t id;
} pocketparse_symbol;

/* ** A compact token event: kind, byte span, and flags. Token delivery uses
** this payload rather than dynamically formatted event names. */
typedef struct pocketparse_token {
    uint32_t kind;
    uint32_t start_byte;
    uint32_t end_byte;
    uint32_t flags;
} pocketparse_token;

/* ** Open an engine over the host's memory grant. Returns NULL if the grant
** is too small to hold the engine's bookkeeping. */
pocketparse_engine *pocketparse_open(
    pocketparse_configuration configuration
);

/* ** Close an engine, reclaiming everything carved from its grant. The host
** still owns and frees the underlying memory block. */
void pocketparse_close(
    pocketparse_engine *engine
);

/* ** The engine's last fault message, or an empty string if none. */
pocketparse_string pocketparse_fault(
    const pocketparse_engine *engine
);

/* ** Compile a .scm grammar source into a grammar handle. */
pocketparse_result pocketparse_compile_scheme(
    pocketparse_engine  *engine,
    pocketparse_source   source,
    pocketparse_grammar **out_grammar
);

/* ** Load a previously packed binary grammar into a grammar handle. The bytes
** must be a self-describing `.ppg` image produced by
** pocketparse_grammar_serialize (magic + version + ABI tag, then the serialized
** compiled grammar). The reconstructed grammar is allocated from the engine
** arena and is fully usable -- it parses identically to the .scm it was packed
** from. A truncated, mistagged, or version-mismatched image is rejected with a
** fault, never a crash. The mirror image of pocketparse_grammar_serialize. */
pocketparse_result pocketparse_load_binary_grammar(
    pocketparse_engine  *engine,
    pocketparse_source   source,
    pocketparse_grammar **out_grammar
);

/* ** Serialize a compiled grammar into a self-describing binary `.ppg` image:
** a magic, a format version, and an ABI tag, followed by the interned string
** table, the symbol-keyed token and rule IR, the extras policy, and the start
** rule. The bytes are written into the grammar's engine arena and returned
** through out_bytes; the host owns nothing it must free (the arena reclaims
** the buffer with the engine). No IO happens here -- the host writes the bytes
** wherever it likes. The mirror image of pocketparse_load_binary_grammar:
** loading what this produces yields a grammar that parses identically. */
pocketparse_result pocketparse_grammar_serialize(
    const pocketparse_grammar *grammar,
    pocketparse_string        *out_bytes
);

/* ** Emit a flag selecting bundle mode for pocketparse_emit_c. With it set the
** emitter produces a self-contained parser program; cleared it produces a
** reusable module that embeds the packed grammar. */
#define POCKETPARSE_EMIT_BUNDLE 0x1u

/* ** Emit a flag selecting compiled-parser mode for pocketparse_emit_c (W14-3).
** With it set the emitter produces a compiled parser -- the grammar structure
** baked into an explicit, arena-backed frame stack driven by one dispatch loop
** (no C recursion across grammar nesting, so a deeply nested input cannot
** overflow the host C stack) -- that is byte-identical to the bytecode VM. The
** output is a standalone program (it loads the embedded grammar, parses stdin,
** and dumps the CST) built against the pocketparse library, whose shared runtime
** (token matcher, tree builder, extras, cursor) it reuses. This is the embedding
** deliverable and the speed lever: the rule-level interpreter dispatch is
** compiled away. The CLI drives it through `pocketparse c --parser`. */
#define POCKETPARSE_EMIT_PARSER 0x2u

/* ** Emit a flag (used together with POCKETPARSE_EMIT_PARSER) selecting the
** LIBRARY shape of the compiled parser. Cleared, the compiled parser is a
** standalone program with a main() that reads stdin and dumps the CST. Set, the
** same compiled parser is emitted WITHOUT main() or the dump helpers and instead
** exposes two callable entries -- pp_compiled_parser_load(engine, &grammar) to
** load the embedded grammar, and pp_compiled_parser_run(parser) to run the
** compiled dispatch loop over a pushed-and-finished parser -- so a host can link
** the generated translation unit and drive the compiled parser in-process (the
** five-way benchmark times it exactly as it times the VM). The CLI drives it
** through `pocketparse c --parser --parser-lib`. */
#define POCKETPARSE_EMIT_PARSER_LIB 0x4u

/* ** Emit C source for a compiled grammar, returned through out_source as bytes
** carved from the engine arena (the host owns nothing it must free and does the
** file IO). The flags word selects the shape: clear for module mode -- a
** reusable translation unit that embeds the packed grammar (the exact bytes
** pocketparse_grammar_serialize would write) plus an accessor,
** pocketparse_embedded_grammar(), returning those bytes as a pocketparse_string
** for pocketparse_load_binary_grammar; or POCKETPARSE_EMIT_BUNDLE for a
** standalone program that loads the embedded grammar, parses stdin, and dumps
** the CST through the public cursor API. No IO happens here; this is the
** freestanding emitter the CLI's `c` subcommand drives.
**
** Added to the otherwise-frozen contract because the emitter logic must stay
** freestanding (it builds the C text into the engine arena) while only the
** hosted CLI may write files: the host needs a library entry point that returns
** the generated bytes for it to write, and the public header is the only seam
** the CLI sees. */
pocketparse_result pocketparse_emit_c(
    pocketparse_engine        *engine,
    const pocketparse_grammar *grammar,
    uint32_t                   flags,
    pocketparse_string        *out_source
);

/* ** Emit the node-types schema for a compiled grammar as a JSON document,
** returned through out_json as bytes carved from the engine arena (the host owns
** nothing it must free and does the file IO). The schema follows tree-sitter's
** node-types.json shape closely enough that the keys are familiar: a JSON array
** of node entries, one per named rule and named token, each with a "type" name,
** "named": true, and a "fields" object mapping each field name to the named
** types that field may hold ({ "types": [ { "type": ..., "named": ... }, ... ] }).
** Entries with no fields still emit "fields": {}. A rule whose body has unnamed
** positional children also carries a "children" object. The output is valid,
** deterministic JSON in grammar-definition order.
**
** Added to the contract alongside pocketparse_emit_c and for the same reason:
** the schema is built by walking the rule/token IR, which only the freestanding
** library can see, while the host CLI does the file IO. The library returns the
** generated bytes through the public header, the only seam the CLI sees. */
pocketparse_result pocketparse_emit_node_types(
    pocketparse_engine        *engine,
    const pocketparse_grammar *grammar,
    pocketparse_string        *out_json
);

/* ** Release a compiled grammar. */
void pocketparse_grammar_free(
    pocketparse_grammar *grammar
);

/* ** Create a streaming parser for a grammar, starting at a named rule. */
pocketparse_result pocketparse_parser_new(
    pocketparse_engine  *engine,
    pocketparse_grammar *grammar,
    pocketparse_string   start_rule,
    pocketparse_parser **out_parser
);

/* ** Release a parser. */
void pocketparse_parser_free(
    pocketparse_parser *parser
);

/* ** Feed the next chunk of input bytes to a parser. */
pocketparse_result pocketparse_push(
    pocketparse_parser *parser,
    pocketparse_string  bytes
);

/* ** Signal end of input and finish the parse. */
pocketparse_result pocketparse_finish(
    pocketparse_parser *parser
);

/* ** The concrete syntax tree a parser has built, or NULL if none yet. */
const pocketparse_tree *pocketparse_parser_tree(
    const pocketparse_parser *parser
);

/* ** The number of diagnostics a parser has produced. */
uint32_t pocketparse_parser_diagnostic_count(
    const pocketparse_parser *parser
);

/* ** The diagnostic at a given index, or NULL if out of range. */
const pocketparse_diagnostic *pocketparse_parser_diagnostic(
    const pocketparse_parser *parser,
    uint32_t                  index
);

/* ** An opaque, heap-carved tree cursor: the ergonomic navigation handle. */
typedef struct pocketparse_tree_cursor pocketparse_tree_cursor;

/* ** Create a cursor over a tree. */
pocketparse_result pocketparse_tree_cursor_new(
    const pocketparse_tree   *tree,
    pocketparse_tree_cursor **out_cursor
);

/* ** Release a cursor. */
void pocketparse_tree_cursor_free(
    pocketparse_tree_cursor *cursor
);

/* ** Rebind a cursor to a (possibly different) tree, at its root. */
void pocketparse_tree_cursor_reset(
    pocketparse_tree_cursor *cursor,
    const pocketparse_tree  *tree
);

/* ** Reposition a cursor at the root of its current tree. */
void pocketparse_tree_cursor_reset_to_root(
    pocketparse_tree_cursor *cursor
);

/* ** The kind symbol of the node under the cursor. */
pocketparse_symbol pocketparse_tree_cursor_kind(
    const pocketparse_tree_cursor *cursor
);

/* ** The printable name of a symbol within a tree. */
pocketparse_string pocketparse_tree_symbol_name(
    const pocketparse_tree *tree,
    pocketparse_symbol      symbol
);

/* ** The field symbol the node under the cursor is bound to in its parent, or a
** symbol whose id is POCKETPARSE_NO_FIELD when the node carries no field. The
** design records a field name on every field-bound child ("Node kinds carry
** their rule/token name; fields carry their field name"), and cursor-only
** access is the sole public way to reach the tree, so a host that renders or
** queries fields -- the CLI tree dump among them -- has no other route to the
** field name. Added to the otherwise-frozen contract for that reason. */
pocketparse_symbol pocketparse_tree_cursor_field(
    const pocketparse_tree_cursor *cursor
);

/* ** The sentinel field id meaning "this node carries no field". */
#define POCKETPARSE_NO_FIELD 0xFFFFFFFFu

/* ** The start byte offset of the node under the cursor. */
uint32_t pocketparse_tree_cursor_start_byte(
    const pocketparse_tree_cursor *cursor
);

/* ** The end byte offset of the node under the cursor. */
uint32_t pocketparse_tree_cursor_end_byte(
    const pocketparse_tree_cursor *cursor
);

/* ** The child count of the node under the cursor. */
uint32_t pocketparse_tree_cursor_child_count(
    const pocketparse_tree_cursor *cursor
);

/* ** The depth of the node under the cursor below the root. */
uint32_t pocketparse_tree_cursor_depth(
    const pocketparse_tree_cursor *cursor
);

/* ** A node flag marking a synthesized "missing" node: a terminal the grammar
** expected but the input never supplied, inserted by recovery as a zero-width
** placeholder so the parse can complete structurally. The data model lists
** "missing/inserted recovery tokens" among the facts the tree must carry, and a
** cursor-only host (the CLI tree dump among them) has no other way to learn that
** a node is a recovery insertion. Added to the otherwise-frozen contract so the
** host can render such nodes distinctly. */
#define POCKETPARSE_NODE_MISSING 0x1u

/* ** The flag word of the node under the cursor (a bitwise OR of
** POCKETPARSE_NODE_* flags). Zero for an ordinary node. Companion to
** POCKETPARSE_NODE_MISSING: the only public route to a node's recovery state. */
uint32_t pocketparse_tree_cursor_flags(
    const pocketparse_tree_cursor *cursor
);

/* ** The index of the node under the cursor within its parent. */
uint32_t pocketparse_tree_cursor_index_in_parent(
    const pocketparse_tree_cursor *cursor
);

/* ** Move the cursor to the parent; returns nonzero on success. */
int pocketparse_tree_cursor_goto_parent(
    pocketparse_tree_cursor *cursor
);

/* ** Move the cursor to the first child; returns nonzero on success. */
int pocketparse_tree_cursor_goto_first_child(
    pocketparse_tree_cursor *cursor
);

/* ** Move the cursor to the last child; returns nonzero on success. */
int pocketparse_tree_cursor_goto_last_child(
    pocketparse_tree_cursor *cursor
);

/* ** Move the cursor to the next sibling; returns nonzero on success. */
int pocketparse_tree_cursor_goto_next_sibling(
    pocketparse_tree_cursor *cursor
);

/* ** Move the cursor to the previous sibling; returns nonzero on success. */
int pocketparse_tree_cursor_goto_previous_sibling(
    pocketparse_tree_cursor *cursor
);

/* ** Move the cursor to a child by index; returns nonzero on success. */
int pocketparse_tree_cursor_goto_child(
    pocketparse_tree_cursor *cursor,
    uint32_t                 child_index
);

/* ** Move to the first child carrying a named field; nonzero on success. */
int pocketparse_tree_cursor_goto_first_child_by_field(
    pocketparse_tree_cursor *cursor,
    pocketparse_string       field
);

/* ** Move to the first child carrying a field by id; nonzero on success. */
int pocketparse_tree_cursor_goto_first_child_by_field_id(
    pocketparse_tree_cursor *cursor,
    pocketparse_symbol       field
);

/* ** Clone a cursor's full state into a freshly allocated cursor. */
pocketparse_result pocketparse_tree_cursor_clone(
    const pocketparse_tree_cursor *source,
    pocketparse_tree_cursor      **out_cursor
);

/* ** Copy one cursor's state over another's. */
void pocketparse_tree_cursor_assign(
    pocketparse_tree_cursor       *target,
    const pocketparse_tree_cursor *source
);

/* ** A stack-allocatable cursor for allocation-sensitive hosts. Its fields
** are public so the host can place it on the stack; treat them as opaque
** and drive it through the cursor functions. */
typedef struct pocketparse_tree_cursor_storage {
    const pocketparse_tree *tree;
    uint32_t                current_node_index;
    uint32_t                parent_node_index;
    uint32_t                child_index;
    uint32_t                depth;
    uint32_t                first_child;
    uint32_t                child_count;
    uint32_t                flags;
    uint32_t                reserved[8];
} pocketparse_tree_cursor_storage;

/* ** Initialise stack-allocated cursor storage at a tree's root. */
void pocketparse_tree_cursor_init(
    pocketparse_tree_cursor_storage *storage,
    const pocketparse_tree          *tree
);

/* ** Extension mechanisms. Plugs are message-style host boundaries (the engine
** calls the plug's emit with a message); bindings are direct named host
** functions. */
typedef enum pocketparse_binding_kind {
    POCKETPARSE_BIND_CLASS,
    POCKETPARSE_BIND_PREDICATE,
    POCKETPARSE_BIND_ACTION
} pocketparse_binding_kind;

/* ** A message crossing a plug boundary. */
typedef struct pocketparse_message {
    pocketparse_string name;
    pocketparse_string target;
    pocketparse_string data;
    uint32_t           flags;
} pocketparse_message;

/* ** A host plug: a named sink for messages, with user context. */
typedef struct pocketparse_plug {
    pocketparse_string name;
    pocketparse_result (*emit)(
        void                      *user,
        const pocketparse_message *message
    );
    void *user;
} pocketparse_plug;

/* ** Install a plug on an engine. */
pocketparse_result pocketparse_plug_install(
    pocketparse_engine     *engine,
    const pocketparse_plug *plug
);

/* ** A direct host binding: a named function of a given kind. */
typedef struct pocketparse_binding {
    pocketparse_string       name;
    pocketparse_binding_kind kind;
    void                    *function;
    void                    *user;
} pocketparse_binding;

/* ** The function a POCKETPARSE_BIND_CLASS binding installs: a byte-class
** membership test. The matcher calls it for the single input byte a (class NAME)
** token primitive faces and treats a nonzero return as "this byte is in the
** class". user is the binding's user pointer, passed back unchanged; byte is the
** candidate input byte (0..255). The function must be pure and reentrant -- the
** scanner may probe the same byte repeatedly while backtracking -- and must not
** allocate or touch IO, so the freestanding core stays freestanding.
**
** Added to the otherwise-frozen contract because the design's host bindings
** ("(class xid-start) ... may be built-ins or host-bound classes") leave the
** binding's concrete C signature unstated, while pocketparse_binding carries the
** function only as an untyped void *. A CLASS binding has one fixed shape; naming
** it here gives the host a checked type to cast through and documents the exact
** contract the matcher relies on. PREDICATE and ACTION bindings (later tranches)
** will name their own signatures the same way. */
typedef int (*pocketparse_class_fn)(void *user, uint32_t byte);

/* ** Register a host binding on an engine. The binding is copied into the engine
** arena, so the host need not keep its pocketparse_binding alive. A CLASS binding
** (kind POCKETPARSE_BIND_CLASS) names a character class a (class NAME) token form
** resolves to; its function must be a pocketparse_class_fn. Re-binding a name of
** the same kind replaces the previous binding. */
pocketparse_result pocketparse_bind(
    pocketparse_engine        *engine,
    const pocketparse_binding *binding
);

/* ** A synchronous module resolver: the host's seam for turning a module name
** named by an (import NAME) grammar form into the module's grammar bytes. The
** core has no filesystem -- "The core has no filesystem, so imports are
** host-mediated [...] The core only sees bytes" -- so it cannot read a module
** itself; it hands the resolver the bare module name and the host fills in
** *out_source with the bytes (a name for diagnostics plus the .scm bytes,
** typically owned by the host for the duration of the compile). Returning
** POCKETPARSE_OK means the module was found and *out_source is valid; any other
** code reports a failure the compiler turns into a fault. The resolver must be
** synchronous: import resolution happens inline during compile, before any
** grammar exists. The fuller async plug/port module path is a later tranche;
** this minimal seam is what the freestanding core needs to obtain the bytes.
**
** Added to the otherwise-frozen contract for that reason: module loading is
** host-mediated by design, and a freestanding library that never touches IO
** has no other way to obtain a module's bytes than a host-supplied callback. */
typedef pocketparse_result (*pocketparse_module_resolver)(
    void               *user,
    pocketparse_string  name,
    pocketparse_source *out_source
);

/* ** Install the module resolver an engine uses while compiling (import NAME)
** forms. resolve may be NULL to clear it, in which case a grammar that imports
** anything is rejected with a fault. user is passed back to the resolver
** unchanged. Companion to pocketparse_module_resolver above. */
pocketparse_result pocketparse_set_module_resolver(
    pocketparse_engine          *engine,
    pocketparse_module_resolver  resolve,
    void                        *user
);

/* ** A registered embedded language, the runtime seam for native nesting (W17-3).
** A grammar's (inject grammar: NAME content: REGION) form names an embedded
** language NAME; the inject runtime looks NAME up in the engine's language
** registry to obtain the parser that re-parses the region. An entry carries the
** embedded grammar (whose interner names the spliced nodes) and an OPTIONAL parse
** function. When `parse' is NULL the region is parsed by the in-process bytecode
** VM over `grammar' -- the dynamic case, byte-identical to today. When `parse' is
** set it is a SEPARATELY-COMPILED generated parser's run entry (the `<name>_run'
** the emitter produces): the inject runtime hands it a sub-parser scoped to the
** region (sharing the host arena) and splices the tree it builds. This is native
** nesting -- a generated host parser invoking a generated embedded parser -- with
** the host binary naming the language but never linking the embedded parser
** directly: the registration wires them at runtime. The host tree is isolated
** from a failing embedded parse either way (the region is spliced under a
** language-tagged injection node regardless). */
typedef struct pocketparse_language_entry {
    pocketparse_grammar *grammar;
    pocketparse_result (*parse)(pocketparse_parser *parser);
} pocketparse_language_entry;

/* ** Register an embedded language under a name on the engine's language registry,
** so an (inject grammar: NAME ...) form resolves NAME to `entry' at parse time.
** NAME is typically the embedded grammar's module name (e.g. "std.javascript"),
** matching the name the host grammar's inject form carries. Registering a name
** already present replaces it. The name bytes are copied into the engine arena;
** `entry' is copied by value (the grammar pointer and parse function are retained,
** so they must outlive the engine's parses). Returns POCKETPARSE_OK, or
** POCKETPARSE_INVALID on a NULL engine/entry, or POCKETPARSE_OUT_OF_MEMORY. A
** language found here takes precedence over a grammar resolved at compile time
** through (inject grammar: NAME); the compile-time embedded grammar stays the
** fallback when no registration matches, so existing in-grammar injection is
** unchanged. */
pocketparse_result pocketparse_register_language(
    pocketparse_engine               *engine,
    pocketparse_string                name,
    const pocketparse_language_entry *entry
);

/* ** Set a compile-time grammar parameter to a truthy value, the host seam for
** the CLI's --param flag. Parameters gate productions written with the
** (when PARAM: X) combinator: while PARAM is set, X is part of the language; with
** PARAM unset, X vanishes from its sequence or choice and the grammar still
** compiles. A value of 0 clears the parameter; any other value sets it. Setting a
** name already set updates it in place. Parameters are read when a grammar is
** compiled (pocketparse_compile_scheme), so install them before compiling. The
** name bytes are copied into the engine arena, so the host need not keep them. */
pocketparse_result pocketparse_set_param(
    pocketparse_engine *engine,
    pocketparse_string  name,
    int                 value
);

/* ** A query is a parser over the linearized tree. The tree is flattened to an
** ENTER/LEAF/EXIT token stream and a query pattern is matched by parsing it (see
** the design's "Queries as parsers over linearized trees"). These two entry
** points expose that machinery: pocketparse_linearize streams the flattened
** tree, and pocketparse_query runs a .scm query and reports its captures. Both
** report through a callback so the freestanding core never does IO; both allocate
** only from the engine's arena and leave the tree untouched. */
typedef enum pocketparse_lin_kind {
    POCKETPARSE_LIN_ENTER = 0, /* opens an internal node */
    POCKETPARSE_LIN_LEAF,      /* a whole leaf (token) node */
    POCKETPARSE_LIN_EXIT       /* closes an internal node */
} pocketparse_lin_kind;

/* ** One token of the linearized stream as reported to a host. kind tags
** ENTER/LEAF/EXIT; node_kind is the node's kind name; field is its field name (or
** empty); text is the leaf's bytes for a LEAF (empty otherwise), sliced from the
** input the caller supplies; start_byte/end_byte are the node's byte span. */
typedef struct pocketparse_lin_token {
    pocketparse_lin_kind kind;
    pocketparse_string   node_kind;
    pocketparse_string   field;
    pocketparse_string   text;
    uint32_t             start_byte;
    uint32_t             end_byte;
} pocketparse_lin_token;

/* ** Flatten a tree to its ENTER/LEAF/EXIT stream, reporting each token through
** `emit` in pre-order. `input` is the parsed bytes, used to slice a LEAF's text;
** pass an empty string to omit leaf text. Returns POCKETPARSE_OK on success. */
pocketparse_result pocketparse_linearize(
    pocketparse_engine     *engine,
    const pocketparse_tree *tree,
    pocketparse_string      input,
    void (*emit)(void *user, const pocketparse_lin_token *token),
    void                   *user
);

/* ** One capture reported by a query match: the @capture name (without the `@'),
** the matched node's byte span, and its text sliced from the input. */
typedef struct pocketparse_query_capture {
    pocketparse_string name;
    uint32_t           start_byte;
    uint32_t           end_byte;
    pocketparse_string text;
} pocketparse_query_capture;

/* ** Run a .scm query over a tree, reporting each capture of every accepted match
** through `emit`. `query_source` is the query bytes; `input` is the parsed bytes
** (to slice capture text). The built-in `#eq?` and `#match?` predicates filter a
** match by byte comparison or the engine's regex, with no host binding. Returns
** POCKETPARSE_OK on a clean run (including zero matches) or POCKETPARSE_INVALID on
** a malformed query. */
pocketparse_result pocketparse_query(
    pocketparse_engine     *engine,
    const pocketparse_tree *tree,
    pocketparse_string      query_source,
    pocketparse_string      input,
    void (*emit)(void *user, const pocketparse_query_capture *capture),
    void                   *user
);

/* ** The pocketparse version string, e.g. "0.1.0". */
const char *pocketparse_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* POCKETPARSE_H */
