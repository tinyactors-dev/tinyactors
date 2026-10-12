/*
 * Prints staticjs's built-in name table, the one builtins.c registers and
 * sjs_name_id searches: every name, the receivers it has a method for, and
 * the method's ID. For the list of what is built and what is not, see
 * docs/builtins-status.md.
 *
 *   builtin-table dump     every entry, one per line, in the table's order,
 *                          then the function table after a header line
 *   builtin-table          a REPL: reads a name per line from stdin and
 *                          prints its name entry and its function entries,
 *                          or "not registered"
 *
 * A name entry is four tab-separated fields:
 *   name      the property name
 *   flags     the SJS_NAME_* bits, as words, comma separated, or "-"
 *   methods   kind=method-id for each receiver kind with a method, and
 *             field=getter for a Temporal value's getter
 *             (sjs_temporal_field), or "-"
 *   mutation  the method ID that changes its receiver, or "-"
 *
 * A function entry is three: the receiver ("" for a global), the name and
 * the built-in ID. The function section starts with the header line
 * "# functions (receiver, name, id)".
 *
 * Method IDs are the numbers of sjs_builtin_id; the C names are not kept at
 * run time, so they are not printed.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../src/internal.h"

typedef struct {
    uint32_t bit;
    const char *word;
} flag_word;

static const flag_word FLAG_WORDS[] = {
    { SJS_NAME_OBJECT, "object" },     { SJS_NAME_ON_ARRAY, "array" },   { SJS_NAME_ON_STRING, "string" },
    { SJS_NAME_ON_SET, "set" },        { SJS_NAME_ON_MAP, "map" },       { SJS_NAME_ON_ITERATOR, "iterator" },
    { SJS_NAME_ON_REGEXP, "regexp" },  { SJS_NAME_MUTATING, "mutating" },
};

/* The receiver kinds in the order of a name's method slots, with their labels. */
static const struct {
    sjs_kind kind;
    const char *label;
} KINDS[] = {
    { SJS_ARRAY, "array" },   { SJS_STRING, "string" }, { SJS_SET, "set" },         { SJS_BIGINT, "bigint" },
    { SJS_MAP, "map" },       { SJS_ITERATOR, "iterator" }, { SJS_REGEXP, "regexp" }, { SJS_NUMBER, "number" },
    { SJS_TEMPORAL, "temporal" },
};

static void print_entry(const sjs_name *entry) {
    printf("%.*s\t", (int)entry->name.length, entry->name.ptr);

    int any = 0;
    for (size_t i = 0; i < sizeof FLAG_WORDS / sizeof FLAG_WORDS[0]; i++) {
        if ((entry->flags & FLAG_WORDS[i].bit) == 0) continue;
        printf("%s%s", any ? "," : "", FLAG_WORDS[i].word);
        any = 1;
    }
    printf("%s\t", any ? "" : "-");

    any = 0;
    for (size_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++) {
        sjs_builtin_id id = sjs_method_of(entry, KINDS[i].kind);
        if (id == SJS_BUILTIN_NONE) continue;
        printf("%s%s=%u", any ? "," : "", KINDS[i].label, (unsigned)id);
        any = 1;
    }
    if (entry->field != 0) {
        printf("%sfield=%u", any ? "," : "", (unsigned)entry->field);
        any = 1;
    }
    printf("%s\t", any ? "" : "-");

    sjs_builtin_id mutation = sjs_mutation_named(entry);
    if (mutation == SJS_BUILTIN_NONE) printf("-\n");
    else printf("%u\n", (unsigned)mutation);
}

/* A function-table entry: the receiver ("" for a global), the name, the ID. */
static void print_function(const sjs_function_entry *entry) {
    printf("%s\t%s\t%u\n", entry->space, entry->name, (unsigned)entry->id);
}

static void print_function_header(void) {
    printf("# functions (receiver, name, id)\n");
}

static int dump(void) {
    uint32_t count;
    const sjs_name *names = sjs_names(&count);
    for (uint32_t i = 0; i < count; i++) print_entry(&names[i]);
    uint32_t functions;
    const sjs_function_entry *table = sjs_functions(&functions);
    print_function_header();
    for (uint32_t i = 0; i < functions; i++) print_function(&table[i]);
    return 0;
}

/* Reads one line into `text`, without its line break and trailing blanks.
 * Returns the length, or -1 at end of input. */
static long read_name(char *text, size_t size) {
    if (fgets(text, (int)size, stdin) == NULL) return -1;
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' || text[length - 1] == ' ' ||
                          text[length - 1] == '\t'))
        text[--length] = '\0';
    size_t start = 0;
    while (text[start] == ' ' || text[start] == '\t') start++;
    memmove(text, text + start, length - start + 1u);
    return (long)(length - start);
}

static int repl(void) {
    char text[256];
    int interactive = isatty(0);
    uint32_t count;
    const sjs_name *names = sjs_names(&count);
    uint32_t functions;
    const sjs_function_entry *table = sjs_functions(&functions);
    for (;;) {
        if (interactive) {
            printf("> ");
            fflush(stdout);
        }
        long length = read_name(text, sizeof text);
        if (length < 0) break;
        if (length == 0) continue;
        uint16_t id = sjs_name_id((sjs_str){ text, (uint32_t)length });
        int found = 0;
        if (id != 0) {
            print_entry(&names[id - 1u]);
            found = 1;
        }
        int header = 0;
        for (uint32_t i = 0; i < functions; i++) {
            if (strcmp(table[i].name, text) != 0) continue;
            if (!header) print_function_header();
            print_function(&table[i]);
            header = 1;
            found = 1;
        }
        if (!found) printf("not registered\n");
    }
    if (interactive) printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return repl();
    if (argc == 2 && strcmp(argv[1], "dump") == 0) return dump();
    fprintf(stderr, "usage: builtin-table [dump]\n");
    return 2;
}
