/*
 * The release's smoke test (mise run release): a program an embedder could
 * write, linked against a packed libtxml (static or shared) and run on the
 * target, including under qemu for the other Linux architecture. It parses a
 * small document through the public API and prints one line; any other output
 * or a non-zero exit fails the release.
 */
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "txml.h"

static void *heap_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    return aligned_alloc(alignment, (size + alignment - 1u) / alignment * alignment);
}

static void heap_free(void *self, const txml_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const txml_allocator_vtable heap_vtable = { heap_alloc, heap_free };

int main(void) {
    static const char text[] = "<list><item>a</item><item>b</item><other/><item>c</item></list>";
    txml_parser_config config = {
        .allocator = { NULL, &heap_vtable },
        .limits = TXML_LIMITS_DEFAULT,
    };
    txml_parser *parser = NULL;
    if (txml_parser_init(&parser, &config) != TXML_OK) {
        fprintf(stderr, "txml_parser_init failed\n");
        return 1;
    }
    txml_bytes input = { (const uint8_t *)text, (uint32_t)strlen(text) };
    bool fed = false;
    for (;;) {
        txml_step step = txml_parse_step(parser);
        if (step == TXML_STEP_NEED_INPUT && !fed) {
            txml_parse_feed(parser, input, true);
            fed = true;
        } else if (step == TXML_STEP_PROGRESS) {
            continue;
        } else if (step == TXML_STEP_DONE) {
            break;
        } else {
            fprintf(stderr, "parse failed at step %d\n", (int)step);
            return 1;
        }
    }
    txml_doc *doc = NULL;
    if (txml_parser_finish(parser, &doc) != TXML_OK) {
        fprintf(stderr, "txml_parser_finish failed\n");
        return 1;
    }
    uint32_t item = txml_doc_find_name(doc, &(txml_qname){ TXML_STR(""), TXML_STR("item") });
    txml_find query = { .name = item, .bound = TXML_INDEX_NONE };
    txml_cursor cursor = txml_doc_root(doc);
    unsigned items = 0;
    while (txml_find_next(&cursor, &query) == TXML_MOVED) {
        items++;
    }
    txml_doc_destroy(doc);
    uint32_t version = txml_version();
    printf("txml %u.%u.%u: %u items\n", (unsigned)(version >> 16), (unsigned)((version >> 8) & 0xffu),
           (unsigned)(version & 0xffu), items);
    return items == 3 ? 0 : 1;
}
