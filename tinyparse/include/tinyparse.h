/*
** tinyparse.h: the public interface of the tinyparse library.
**
** The library compiles a grammar (.scm text) into a parser that builds a
** concrete syntax tree and diagnostics. It is freestanding C99: this header
** needs only <stddef.h> and <stdint.h>, and the library allocates only from the
** memory grant the host passes to tinyparse_open. It does no I/O, reads no
** clock, starts no thread and keeps no writable static data.
**
** Every string is a tinyparse_string: a pointer and a length, never
** NUL-terminated. docs/reference/c-api.md is the full reference: result codes,
** ownership, threading, memory sizing and the generated parser interface.
**
** Declared and unused: TINYPARSE_DONE and TINYPARSE_ERROR (never returned),
** tinyparse_configuration.flags, TINYPARSE_BIND_PREDICATE and
** TINYPARSE_BIND_ACTION, tinyparse_point, tinyparse_range and tinyparse_token.
*/
#ifndef TINYPARSE_H
#define TINYPARSE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ** Marks every public function. Empty unless defined before this header;
** the shared-library build defines it to give the functions default
** visibility. */
#ifndef TINYPARSE_API
#define TINYPARSE_API
#endif

/* ** A byte string. length is authoritative; no terminating NUL is needed.
** An empty string has length 0 and may have a NULL pointer. */
typedef struct tinyparse_string {
    const uint8_t *pointer;
    uint32_t       length;
} tinyparse_string;

/* ** The host's memory grant. base (not NULL, 16-byte aligned) and size are
** the first block. grow is NULL for a fixed budget; otherwise it returns a new
** block of at least need bytes, or NULL, and may store a larger usable size in
** *got (which holds need on entry). The engine never resizes, moves or frees a
** block. chunk_minimum is the smallest chunk taken from a block; 0 selects
** 4096. */
typedef struct tinyparse_memory {
    void   *base;
    size_t  size;
    void  *(*grow)(void *user, size_t need, size_t *got);
    void   *user;
    size_t  chunk_minimum;
} tinyparse_memory;

/* ** Arguments of tinyparse_open. flags is reserved: set it to 0. */
typedef struct tinyparse_configuration {
    tinyparse_memory memory;
    uint32_t         flags;
} tinyparse_configuration;

/* ** Result codes. MORE comes only from a generated parser's P_run_chunk.
** DONE and ERROR are never returned. A failed compile or load reports a spent
** grant as INVALID with an "out of memory" fault. */
typedef enum tinyparse_result {
    TINYPARSE_OK = 0,
    TINYPARSE_MORE,
    TINYPARSE_DONE,
    TINYPARSE_OUT_OF_MEMORY,
    TINYPARSE_INVALID,
    TINYPARSE_SYNTAX_ERROR,
    TINYPARSE_ERROR
} tinyparse_result;

/* ** Opaque handles. */
typedef struct tinyparse_engine  tinyparse_engine;
typedef struct tinyparse_grammar tinyparse_grammar;
typedef struct tinyparse_parser  tinyparse_parser;
typedef struct tinyparse_tree    tinyparse_tree;

/* ** Grammar text or a packed grammar image. The library does not read
** name. */
typedef struct tinyparse_source {
    tinyparse_string name;
    tinyparse_string bytes;
} tinyparse_source;

/* ** A 0-based row and column. No function takes or returns it. */
typedef struct tinyparse_point {
    uint32_t row;
    uint32_t column;
} tinyparse_point;

/* ** A byte range with points. No function takes or returns it. */
typedef struct tinyparse_range {
    uint32_t        start_byte;
    uint32_t        end_byte;
    tinyparse_point start_point;
    tinyparse_point end_point;
} tinyparse_range;

/* ** Diagnostic severity. The library produces ERROR only. */
typedef enum tinyparse_severity {
    TINYPARSE_SEVERITY_NOTE = 0,
    TINYPARSE_SEVERITY_WARNING,
    TINYPARSE_SEVERITY_ERROR
} tinyparse_severity;

/* ** A byte range of the input, end-exclusive, with a message. flags is 0. */
typedef struct tinyparse_label {
    uint32_t         start_byte;
    uint32_t         end_byte;
    tinyparse_string message;
    uint32_t         flags;
} tinyparse_label;

/* ** One diagnostic of a parse. code names it (docs/reference/diagnostics.md);
** every diagnostic has one label; help comes from a grammar's recover form or
** is empty; note is empty. The library never prints a diagnostic. */
typedef struct tinyparse_diagnostic {
    tinyparse_severity     severity;
    tinyparse_string       code;
    tinyparse_string       message;
    const tinyparse_label *labels;
    uint32_t               label_count;
    tinyparse_string       help;
    tinyparse_string       note;
} tinyparse_diagnostic;

/* ** A node kind or field name of a tree; tinyparse_tree_symbol_name gives
** its text. */
typedef struct tinyparse_symbol {
    uint32_t id;
} tinyparse_symbol;

/* ** A token span. No function takes or returns it. */
typedef struct tinyparse_token {
    uint32_t kind;
    uint32_t start_byte;
    uint32_t end_byte;
    uint32_t flags;
} tinyparse_token;

/* ** Open an engine over a memory grant. Returns NULL when memory.base is
** NULL, memory.size is 0, or the block cannot hold the engine plus one
** chunk. */
TINYPARSE_API tinyparse_engine *tinyparse_open(
    tinyparse_configuration configuration
);

/* ** Releases nothing. After the last call on the engine, the host frees base
** and every block grow returned. */
TINYPARSE_API void tinyparse_close(
    tinyparse_engine *engine
);

/* ** The engine's fault: the reason a call failed, for the calls that set one.
** Empty when none was set. Only tinyparse_compile_scheme and
** tinyparse_load_binary_grammar clear it, on success. */
TINYPARSE_API tinyparse_string tinyparse_fault(
    const tinyparse_engine *engine
);

/* ** Compile grammar text. Install the module resolver and set parameters
** first: import, extends and inject names are resolved, and parameters read,
** during this call. Returns OK, or INVALID with a fault (none for a NULL
** engine); a spent grant is INVALID too. The grammar keeps pointers into
** source.bytes and into the bytes the resolver returned; keep them valid while
** the grammar is in use. */
TINYPARSE_API tinyparse_result tinyparse_compile_scheme(
    tinyparse_engine   *engine,
    tinyparse_source    source,
    tinyparse_grammar **out_grammar
);

/* ** Load a packed grammar image (.tpg) written by tinyparse_grammar_serialize
** or `tinyparse pack`. The image carries parameters and composed grammars, so
** no resolver is called. The grammar copies the image. Returns OK or INVALID
** with a fault for a short, mistagged, mismatched or corrupt image. */
TINYPARSE_API tinyparse_result tinyparse_load_binary_grammar(
    tinyparse_engine   *engine,
    tinyparse_source    source,
    tinyparse_grammar **out_grammar
);

/* ** Pack a grammar into the .tpg image tinyparse_load_binary_grammar reads.
** The bytes live in the engine until the grant is freed. */
TINYPARSE_API tinyparse_result tinyparse_grammar_serialize(
    const tinyparse_grammar *grammar,
    tinyparse_string        *out_bytes
);

/* ** tinyparse_emit_c flag: a program that parses standard input with the VM
** and prints the tree. Without flags, tinyparse_emit_c writes a module that
** embeds the packed grammar and defines tinyparse_embedded_grammar(). */
#define TINYPARSE_EMIT_BUNDLE 0x1u

/* ** tinyparse_emit_c flag: a generated parser, one C function per rule and
** token, that builds the tree the VM builds. Nesting deeper than 1000 rule
** calls fails with a syntax error rather than overflowing the C stack. Alone,
** the output is a program that parses standard input (`tinyparse c --parser`).
** Takes precedence over TINYPARSE_EMIT_BUNDLE. */
#define TINYPARSE_EMIT_PARSER 0x2u

/* ** tinyparse_emit_c flag, together with TINYPARSE_EMIT_PARSER: emit the
** generated parser with no main() and with the entries <prefix>_load,
** <prefix>_run and, when the start rule repeats top-level items,
** <prefix>_run_chunk (`tinyparse c --parser-lib`). The prefix is the grammar's
** name. Ignored without TINYPARSE_EMIT_PARSER. */
#define TINYPARSE_EMIT_PARSER_LIB 0x4u

/* ** Emit C source for a grammar; flags select the output (see the
** TINYPARSE_EMIT_ flags). The text lives in the engine until the grant is
** freed; the library does no I/O. */
TINYPARSE_API tinyparse_result tinyparse_emit_c(
    tinyparse_engine        *engine,
    const tinyparse_grammar *grammar,
    uint32_t                 flags,
    tinyparse_string        *out_source
);

/* ** Emit the grammar's node types as JSON, the document `tinyparse
** node-types` prints: an array with one object per named rule and token, in
** definition order, each with "type", "named", "fields" and, for a rule with
** unnamed children, "children". The bytes live in the engine. */
TINYPARSE_API tinyparse_result tinyparse_emit_node_types(
    tinyparse_engine        *engine,
    const tinyparse_grammar *grammar,
    tinyparse_string        *out_json
);

/* ** Releases nothing; the grammar stays allocated until the grant is
** freed. */
TINYPARSE_API void tinyparse_grammar_free(
    tinyparse_grammar *grammar
);

/* ** Create a parser. An empty start_rule selects the grammar's start: rule; a
** name the grammar does not have returns INVALID with a fault. out_parser
** must not be NULL. */
TINYPARSE_API tinyparse_result tinyparse_parser_new(
    tinyparse_engine   *engine,
    tinyparse_grammar  *grammar,
    tinyparse_string    start_rule,
    tinyparse_parser  **out_parser
);

/* ** Return the parser's memory (input, tree, diagnostics, cursors) to the
** engine for later parses. The parser, its tree, its diagnostics and every
** cursor over the tree are invalid afterward. */
TINYPARSE_API void tinyparse_parser_free(
    tinyparse_parser *parser
);

/* ** Append input bytes. The parser copies them and parses nothing until
** tinyparse_finish. INVALID once the parser has finished. */
TINYPARSE_API tinyparse_result tinyparse_push(
    tinyparse_parser *parser,
    tinyparse_string  bytes
);

/* ** Parse the pushed input with the VM. OK when the input matched, possibly
** after recovery; SYNTAX_ERROR when it did not. A second call does not parse
** again; it returns OK whenever a tree exists, even after SYNTAX_ERROR, so keep
** the first result. */
TINYPARSE_API tinyparse_result tinyparse_finish(
    tinyparse_parser *parser
);

/* ** The parser's tree, or NULL. After SYNTAX_ERROR it can hold the nodes
** built before the failure. Valid until tinyparse_parser_free. */
TINYPARSE_API const tinyparse_tree *tinyparse_parser_tree(
    const tinyparse_parser *parser
);

/* ** The number of diagnostics the parse produced. */
TINYPARSE_API uint32_t tinyparse_parser_diagnostic_count(
    const tinyparse_parser *parser
);

/* ** Diagnostic index, counting from 0, or NULL when out of range. */
TINYPARSE_API const tinyparse_diagnostic *tinyparse_parser_diagnostic(
    const tinyparse_parser *parser,
    uint32_t                index
);

/* ** A cursor: a position on one node of a tree. */
typedef struct tinyparse_tree_cursor tinyparse_tree_cursor;

/* ** Create a cursor on the root. It lives in the parser and is invalid after
** tinyparse_parser_free. */
TINYPARSE_API tinyparse_result tinyparse_tree_cursor_new(
    const tinyparse_tree   *tree,
    tinyparse_tree_cursor **out_cursor
);

/* ** Releases nothing; tinyparse_parser_free releases the cursor. */
TINYPARSE_API void tinyparse_tree_cursor_free(
    tinyparse_tree_cursor *cursor
);

/* ** Move the cursor to the root of tree, which may be another tree. */
TINYPARSE_API void tinyparse_tree_cursor_reset(
    tinyparse_tree_cursor *cursor,
    const tinyparse_tree  *tree
);

/* ** Move the cursor to the root of its tree. */
TINYPARSE_API void tinyparse_tree_cursor_reset_to_root(
    tinyparse_tree_cursor *cursor
);

/* ** The node kind of the node under the cursor. */
TINYPARSE_API tinyparse_symbol tinyparse_tree_cursor_kind(
    const tinyparse_tree_cursor *cursor
);

/* ** The name of a node kind or field symbol, or an empty string. */
TINYPARSE_API tinyparse_string tinyparse_tree_symbol_name(
    const tinyparse_tree *tree,
    tinyparse_symbol      symbol
);

/* ** The field the node holds in its parent, or a symbol whose id is
** TINYPARSE_NO_FIELD. */
TINYPARSE_API tinyparse_symbol tinyparse_tree_cursor_field(
    const tinyparse_tree_cursor *cursor
);

/* ** The id of "no field". */
#define TINYPARSE_NO_FIELD 0xFFFFFFFFu

/* ** The node's start, a 0-based byte offset into the input. */
TINYPARSE_API uint32_t tinyparse_tree_cursor_start_byte(
    const tinyparse_tree_cursor *cursor
);

/* ** The offset one past the node's last byte. */
TINYPARSE_API uint32_t tinyparse_tree_cursor_end_byte(
    const tinyparse_tree_cursor *cursor
);

/* ** The number of children of the node. */
TINYPARSE_API uint32_t tinyparse_tree_cursor_child_count(
    const tinyparse_tree_cursor *cursor
);

/* ** Levels below the root; the root is at depth 0. */
TINYPARSE_API uint32_t tinyparse_tree_cursor_depth(
    const tinyparse_tree_cursor *cursor
);

/* ** Node flag: a MISSING node, the zero-width node a grammar's recover form
** inserts for input that was not there. */
#define TINYPARSE_NODE_MISSING 0x1u

/* ** The node's flags: TINYPARSE_NODE_MISSING or 0. */
TINYPARSE_API uint32_t tinyparse_tree_cursor_flags(
    const tinyparse_tree_cursor *cursor
);

/* ** The node's 0-based position among its parent's children; 0 at the root.
** Costs one step per earlier sibling. */
TINYPARSE_API uint32_t tinyparse_tree_cursor_index_in_parent(
    const tinyparse_tree_cursor *cursor
);

/* ** Move to the parent. Returns 1, or 0 at the root. */
TINYPARSE_API int tinyparse_tree_cursor_goto_parent(
    tinyparse_tree_cursor *cursor
);

/* ** Move to the first child. Returns 1, or 0 when there is none. */
TINYPARSE_API int tinyparse_tree_cursor_goto_first_child(
    tinyparse_tree_cursor *cursor
);

/* ** Move to the last child. Returns 1, or 0 when there is none. */
TINYPARSE_API int tinyparse_tree_cursor_goto_last_child(
    tinyparse_tree_cursor *cursor
);

/* ** Move to the next sibling. Returns 1, or 0 when there is none. */
TINYPARSE_API int tinyparse_tree_cursor_goto_next_sibling(
    tinyparse_tree_cursor *cursor
);

/* ** Move to the previous sibling. Returns 1, or 0 when there is none. */
TINYPARSE_API int tinyparse_tree_cursor_goto_previous_sibling(
    tinyparse_tree_cursor *cursor
);

/* ** Move to child child_index, counting from 0. Returns 1, or 0 when there is
** none. */
TINYPARSE_API int tinyparse_tree_cursor_goto_child(
    tinyparse_tree_cursor *cursor,
    uint32_t               child_index
);

/* ** Move to the first child whose field is named field. Returns 1, or 0. */
TINYPARSE_API int tinyparse_tree_cursor_goto_first_child_by_field(
    tinyparse_tree_cursor *cursor,
    tinyparse_string       field
);

/* ** Move to the first child whose field symbol is field. Returns 1, or 0. */
TINYPARSE_API int tinyparse_tree_cursor_goto_first_child_by_field_id(
    tinyparse_tree_cursor *cursor,
    tinyparse_symbol       field
);

/* ** Create a cursor on the same node at the same depth. It lives in the
** parser. */
TINYPARSE_API tinyparse_result tinyparse_tree_cursor_clone(
    const tinyparse_tree_cursor *source,
    tinyparse_tree_cursor      **out_cursor
);

/* ** Put target on the node and depth of source. */
TINYPARSE_API void tinyparse_tree_cursor_assign(
    tinyparse_tree_cursor       *target,
    const tinyparse_tree_cursor *source
);

/* ** Storage for a cursor that needs no allocation. Pass it to the cursor
** functions as (tinyparse_tree_cursor *)&storage; do not use the fields. */
typedef struct tinyparse_tree_cursor_storage {
    const tinyparse_tree *tree;
    uint32_t              current_node_index;
    uint32_t              parent_node_index;
    uint32_t              child_index;
    uint32_t              depth;
    uint32_t              first_child;
    uint32_t              child_count;
    uint32_t              flags;
    uint32_t              reserved[8];
} tinyparse_tree_cursor_storage;

/* ** Put storage on the root of tree. */
TINYPARSE_API void tinyparse_tree_cursor_init(
    tinyparse_tree_cursor_storage *storage,
    const tinyparse_tree          *tree
);

/* ** Binding kinds. CLASS gives (class NAME) its bytes. PREDICATE and ACTION
** are reserved: tinyparse_bind stores them and nothing uses them. */
typedef enum tinyparse_binding_kind {
    TINYPARSE_BIND_CLASS,
    TINYPARSE_BIND_PREDICATE,
    TINYPARSE_BIND_ACTION
} tinyparse_binding_kind;

/* ** A message sent to a plug. For trace messages, name is rule.enter,
** rule.exit, token, rule.memo-on or rule.replay; data is the rule or token
** name; target is empty; flags is 0. */
typedef struct tinyparse_message {
    tinyparse_string name;
    tinyparse_string target;
    tinyparse_string data;
    uint32_t         flags;
} tinyparse_message;

/* ** A named receiver of messages. The engine sends to one name, "trace",
** during a parse, and ignores what emit returns. */
typedef struct tinyparse_plug {
    tinyparse_string name;
    tinyparse_result (*emit)(
        void                    *user,
        const tinyparse_message *message
    );
    void *user;
} tinyparse_plug;

/* ** Install a plug; the engine copies it. A name installed again replaces the
** earlier plug. */
TINYPARSE_API tinyparse_result tinyparse_plug_install(
    tinyparse_engine     *engine,
    const tinyparse_plug *plug
);

/* ** A host binding: a name, a kind, and a function with its user pointer. */
typedef struct tinyparse_binding {
    tinyparse_string       name;
    tinyparse_binding_kind kind;
    void                  *function;
    void                  *user;
} tinyparse_binding;

/* ** The function of a TINYPARSE_BIND_CLASS binding. Returns nonzero when
** byte (0 to 255) is in the class. It must give the same answer for the same
** byte every time and be safe to call again before an earlier call returns:
** the parser calls it again when it backtracks. */
typedef int (*tinyparse_class_fn)(void *user, uint32_t byte);

/* ** Store a binding; the engine copies it and its name. A name bound again
** with the same kind is replaced. Classes are looked up when a parse runs, so
** a binding made before or after compiling applies to later parses. A class
** with no binding matches no byte. */
TINYPARSE_API tinyparse_result tinyparse_bind(
    tinyparse_engine        *engine,
    const tinyparse_binding *binding
);

/* ** A module resolver: give the grammar text for name, the name written in
** an import, extends or inject form, by setting out_source->bytes and
** returning OK. Any other result fails the compile with a fault. Called during
** tinyparse_compile_scheme only; the bytes must stay valid while the compiled
** grammar is in use. */
typedef tinyparse_result (*tinyparse_module_resolver)(
    void             *user,
    tinyparse_string  name,
    tinyparse_source *out_source
);

/* ** Install the engine's module resolver, or remove it with NULL. Without
** one, a grammar that imports, extends or injects fails to compile. */
TINYPARSE_API tinyparse_result tinyparse_set_module_resolver(
    tinyparse_engine          *engine,
    tinyparse_module_resolver  resolve,
    void                      *user
);

/* ** A language for (inject grammar: NAME ...) regions. grammar parses the
** region, or NULL keeps the grammar the inject form resolved at compile time.
** parse is NULL for the VM, or the <prefix>_run entry of a generated parser
** whose <prefix>_load returned grammar. */
typedef struct tinyparse_language_entry {
    tinyparse_grammar *grammar;
    tinyparse_result (*parse)(tinyparse_parser *parser);
} tinyparse_language_entry;

/* ** Register entry under name, the name an inject form carries. It applies to
** parses started afterward; registering a name again replaces it. The engine
** copies name and entry; grammar and parse must stay valid while parses use
** them. */
TINYPARSE_API tinyparse_result tinyparse_register_language(
    tinyparse_engine               *engine,
    tinyparse_string                name,
    const tinyparse_language_entry *entry
);

/* ** Set (value nonzero) or clear (value 0) the parameter a (when NAME: ...)
** form tests. Read during tinyparse_compile_scheme only, so set it before
** compiling. The engine copies the name. */
TINYPARSE_API tinyparse_result tinyparse_set_param(
    tinyparse_engine *engine,
    tinyparse_string  name,
    int               value
);

/* ** Token kinds of the linearized tree: ENTER and EXIT around a node with
** children, LEAF for a node without. */
typedef enum tinyparse_lin_kind {
    TINYPARSE_LIN_ENTER = 0,
    TINYPARSE_LIN_LEAF,
    TINYPARSE_LIN_EXIT
} tinyparse_lin_kind;

/* ** One linearized token. node_kind and field are names (field may be
** empty); text is a LEAF's slice of the input, empty otherwise. */
typedef struct tinyparse_lin_token {
    tinyparse_lin_kind kind;
    tinyparse_string   node_kind;
    tinyparse_string   field;
    tinyparse_string   text;
    uint32_t           start_byte;
    uint32_t           end_byte;
} tinyparse_lin_token;

/* ** Call emit for each token of the tree in pre-order. input is the parsed
** text, or empty to leave text empty. Each call allocates in the engine until
** the grant is freed. */
TINYPARSE_API tinyparse_result tinyparse_linearize(
    tinyparse_engine     *engine,
    const tinyparse_tree *tree,
    tinyparse_string      input,
    void (*emit)(void *user, const tinyparse_lin_token *token),
    void                 *user
);

/* ** One capture of a query match: the capture name without @, the node's
** byte range, and its slice of the input. */
typedef struct tinyparse_query_capture {
    tinyparse_string name;
    uint32_t         start_byte;
    uint32_t         end_byte;
    tinyparse_string text;
} tinyparse_query_capture;

/* ** Run a query (docs/reference/query-language.md) and call emit for each
** capture of every match. OK with zero or more matches; INVALID for a query
** that does not compile against the tree's grammar, with no fault set. Each
** call allocates in the engine until the grant is freed. */
TINYPARSE_API tinyparse_result tinyparse_query(
    tinyparse_engine     *engine,
    const tinyparse_tree *tree,
    tinyparse_string      query_source,
    tinyparse_string      input,
    void (*emit)(void *user, const tinyparse_query_capture *capture),
    void                 *user
);

/* ** The library version, NUL-terminated, for example "0.1.0". */
TINYPARSE_API const char *tinyparse_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYPARSE_H */
