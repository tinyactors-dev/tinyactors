/*
 * The public surface, one typed pointer per TXML_API function.
 *
 * Assigning a function to a pointer of its expected type is a compile error
 * when the declaration is missing, renamed, or typed differently — which is
 * exactly the check we want while there is no implementation to link against.
 * Casts would defeat it, so there are none here.
 *
 * This file is compiled, never linked: the symbols do not exist yet. When the
 * implementation lands it keeps its value unchanged, so leave it compile-only.
 *
 * Every function declared in include/txml.h must appear below. Fifty-eight
 * today: ADR 0018 withdrew txml_doc_mark and txml_doc_rewind.
 */
#include "../include/txml.h"

struct txml_api_surface {
    /* Arena. */
    txml_result (*arena_init)(txml_arena *, txml_allocator, size_t);
    void *(*arena_alloc)(txml_arena *, size_t, size_t);
    txml_arena_mark (*arena_position)(const txml_arena *);
    void (*arena_release)(txml_arena *, txml_arena_mark);
    void (*arena_reset)(txml_arena *);
    void (*arena_destroy)(txml_arena *);

    /* Parser. */
    txml_result (*parser_init)(txml_parser **, const txml_parser_config *);
    uint32_t (*parse_feed)(txml_parser *, txml_bytes, bool);
    txml_step (*parse_step)(txml_parser *);
    const txml_doc *(*parser_doc)(const txml_parser *);
    txml_result (*parser_error)(const txml_parser *, txml_document_error *);
    uint64_t (*parser_offset)(const txml_parser *);
    txml_result (*parser_finish)(txml_parser *, txml_doc **);
    void (*parser_destroy)(txml_parser *);
    void (*doc_destroy)(txml_doc *);

    /* Document. */
    uint32_t (*doc_find_name)(const txml_doc *, const txml_qname *);
    uint32_t (*doc_name_count)(const txml_doc *);
    txml_result (*doc_name_at)(const txml_doc *, uint32_t, txml_qname *);
    uint32_t (*doc_node_count)(const txml_doc *);
    const txml_arena *(*doc_arena)(const txml_doc *, txml_arena_kind);

    /* Cursors. */
    txml_cursor (*doc_root)(const txml_doc *);
    txml_move (*first_child)(txml_cursor *);
    txml_move (*next_sibling)(txml_cursor *);
    txml_move (*parent)(txml_cursor *);
    txml_move (*next_element)(txml_cursor *);
    uint32_t (*index)(txml_cursor);
    txml_cursor (*cursor_at)(const txml_doc *, uint32_t);
    uint32_t (*subtree_end)(txml_cursor);
    bool (*contains)(txml_cursor, txml_cursor);
    txml_move (*find_next)(txml_cursor *, const txml_find *);
    txml_node_kind (*kind)(txml_cursor);
    uint32_t (*flags)(txml_cursor);
    uint32_t (*depth)(txml_cursor);
    uint32_t (*node_name_id)(txml_cursor);
    txml_str (*node_prefix)(txml_cursor);
    txml_str (*node_text)(txml_cursor);
    txml_result (*node_position)(txml_cursor, txml_position *);
    txml_result (*node_span)(txml_cursor, txml_span *);
    uint32_t (*attribute_count)(txml_cursor);
    txml_move (*first_attribute)(txml_cursor, txml_attr_cursor *);
    txml_move (*next_attribute)(txml_attr_cursor *);
    txml_result (*attribute_read)(txml_attr_cursor, txml_attribute_view *);
    txml_result (*attribute_span_read)(txml_attr_cursor, txml_attribute_span *);
    bool (*find_attribute)(txml_cursor, uint32_t, txml_str *);

    /* Writer. */
    txml_result (*writer_init)(txml_writer **, const txml_writer_config *);
    txml_result (*write_start)(txml_writer *, const txml_write_name *);
    txml_result (*write_attribute)(txml_writer *, const txml_write_attr *);
    txml_result (*write_text)(txml_writer *, txml_str);
    txml_result (*write_cdata)(txml_writer *, txml_str);
    txml_result (*write_comment)(txml_writer *, txml_str);
    txml_result (*write_pi)(txml_writer *, txml_str, txml_str);
    txml_result (*write_end)(txml_writer *);
    txml_result (*write_subtree)(txml_writer *, txml_cursor);
    txml_bytes (*writer_output)(const txml_writer *);
    txml_result (*writer_drain)(txml_writer *);
    txml_result (*writer_finish)(txml_writer *);
    void (*writer_destroy)(txml_writer *);

    /* Text helpers. */
    bool (*str_equal)(txml_str, txml_str);
    bool (*qname_equal)(const txml_qname *, const txml_qname *);
    bool (*str_starts_with)(txml_str, txml_str);
};

/* External linkage on purpose: the surface is the point of the file, and an
 * unused static would be a warning. */
const struct txml_api_surface txml_api_surface = {
    .arena_init = txml_arena_init,
    .arena_alloc = txml_arena_alloc,
    .arena_position = txml_arena_position,
    .arena_release = txml_arena_release,
    .arena_reset = txml_arena_reset,
    .arena_destroy = txml_arena_destroy,

    .parser_init = txml_parser_init,
    .parse_feed = txml_parse_feed,
    .parse_step = txml_parse_step,
    .parser_doc = txml_parser_doc,
    .parser_error = txml_parser_error,
    .parser_offset = txml_parser_offset,
    .parser_finish = txml_parser_finish,
    .parser_destroy = txml_parser_destroy,
    .doc_destroy = txml_doc_destroy,

    .doc_find_name = txml_doc_find_name,
    .doc_name_count = txml_doc_name_count,
    .doc_name_at = txml_doc_name_at,
    .doc_node_count = txml_doc_node_count,
    .doc_arena = txml_doc_arena,

    .doc_root = txml_doc_root,
    .first_child = txml_first_child,
    .next_sibling = txml_next_sibling,
    .parent = txml_parent,
    .next_element = txml_next_element,
    .index = txml_index,
    .cursor_at = txml_cursor_at,
    .subtree_end = txml_subtree_end,
    .contains = txml_contains,
    .find_next = txml_find_next,
    .kind = txml_kind,
    .flags = txml_flags,
    .depth = txml_depth,
    .node_name_id = txml_node_name_id,
    .node_prefix = txml_node_prefix,
    .node_text = txml_node_text,
    .node_position = txml_node_position,
    .node_span = txml_node_span,
    .attribute_count = txml_attribute_count,
    .first_attribute = txml_first_attribute,
    .next_attribute = txml_next_attribute,
    .attribute_read = txml_attribute_read,
    .attribute_span_read = txml_attribute_span_read,
    .find_attribute = txml_find_attribute,

    .writer_init = txml_writer_init,
    .write_start = txml_write_start,
    .write_attribute = txml_write_attribute,
    .write_text = txml_write_text,
    .write_cdata = txml_write_cdata,
    .write_comment = txml_write_comment,
    .write_pi = txml_write_pi,
    .write_end = txml_write_end,
    .write_subtree = txml_write_subtree,
    .writer_output = txml_writer_output,
    .writer_drain = txml_writer_drain,
    .writer_finish = txml_writer_finish,
    .writer_destroy = txml_writer_destroy,

    .str_equal = txml_str_equal,
    .qname_equal = txml_qname_equal,
    .str_starts_with = txml_str_starts_with,
};

/* A new TXML_API function must be registered above: the struct grows and this
 * assertion is what tells you to update the count in the file header. */
_Static_assert(sizeof(struct txml_api_surface) == 60u * sizeof(void (*)(void)),
               "every TXML_API function must appear in txml_api_surface");
