/*
 * What `mise run bench-engines` needs from bench/bench.c, read from
 * bench.c itself so that the other engines run the same cases on the same
 * data. It includes bench.c (its own main renamed) and prints JSON:
 *
 *   dump cases            one line per case: its name, unit, op, sizes,
 *                         smoke size, source (or the module it reads),
 *                         globals (name, fixture, literal) and the value
 *                         it gives at its smoke size (.expect)
 *   dump values NAME N    the case set up at size N by bench.c's own
 *                         make_* functions: each global as staticjs's
 *                         writer writes it, and for an eval case its
 *                         result, each as length and FNV-1a hash (32 bits,
 *                         over UTF-16 code units, as guest.js's __hash)
 *
 * The JavaScript generators in guest.js must give the same hashes
 * (compare.mjs checks them before anything is timed), so a change to a
 * fixture in bench.c fails the comparison instead of measuring other data.
 * Run from staticjs/: bench.c reads bench/modules/.
 */
#define main bench_main
#include "../bench.c"
#undef main

/* Names for bench.c's enums. Designated initializers keep them right when
 * bench.c reorders an enum, and a renamed or removed constant stops this
 * file from compiling; a new fixture prints as null and guest.mjs refuses
 * the case that uses it. */
static const char *const fixture_names[] = {
    [FIX_NONE] = "none",
    [FIX_LITERAL] = "literal",
    [FIX_XS] = "xs",
    [FIX_ROWS] = "rows",
    [FIX_IDS] = "ids",
    [FIX_SET_A] = "setA",
    [FIX_SET_B] = "setB",
    [FIX_PENDING_HALF] = "pendingHalf",
    [FIX_NUMBERS] = "numbers",
    [FIX_STRINGS] = "strings",
    [FIX_TEXT] = "text",
    [FIX_WATERFILL] = "waterFill",
    [FIX_CAPACITY] = "capacity",
    [FIX_DEMAND] = "demand",
    [FIX_NUMBER_TEXTS] = "numberTexts15",
    [FIX_NUMBER_TEXTS_17] = "numberTexts17",
    [FIX_BIGINTS] = "bigints",
    [FIX_ID_MAP] = "idMap",
    [FIX_ROWS_JSON] = "rowsJSON",
};

static const char *const text_names[] = {
    [TEXT_INLINE] = "inline",
    [TEXT_POWER] = "power",
    [TEXT_CAMPAIGN] = "campaign",
    [TEXT_SYNTHETIC] = "synthetic",
};

static const char *const op_names[] = {
    [OP_PARSE] = "parse",
    [OP_COMPILE] = "compile",
    [OP_EVAL] = "eval",
    [OP_WRITE] = "write",
    [OP_VM_FRESH] = "vmFresh",
    [OP_VM_RESET] = "vmReset",
    [OP_VALUES_ROWS] = "valuesRows",
    [OP_VALUES_SET] = "valuesSet",
};

#define NAME_OF(table, value) \
    ((size_t)(value) < sizeof(table) / sizeof(table[0]) ? (table)[(value)] : NULL)

static void print_string(const char *s) {
    if (s == NULL) {
        fputs("null", stdout);
        return;
    }
    putchar('"');
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            putchar('\\');
            putchar(ch);
        } else if (ch == '\n') {
            fputs("\\n", stdout);
        } else if (ch < 0x20) {
            printf("\\u%04x", ch);
        } else {
            putchar(ch);
        }
    }
    putchar('"');
}

static void dump_cases(void) {
    for (size_t i = 0; i < CASE_COUNT; i++) {
        const bench_case *c = &cases[i];
        fputs("{\"name\":", stdout);
        print_string(c->name);
        printf(",\"unit\":\"%s\",\"op\":", unit_names[c->unit]);
        print_string(NAME_OF(op_names, c->op));
        fputs(",\"text\":", stdout);
        print_string(NAME_OF(text_names, c->text));
        printf(",\"smoke\":%u,\"source\":", (unsigned)c->smoke);
        print_string(c->source);
        fputs(",\"expect\":", stdout);
        print_string(c->expect);
        fputs(",\"globals\":[", stdout);
        for (int g = 0; g < MAX_GLOBALS && c->globals[g].name != NULL; g++) {
            fputs(g == 0 ? "{\"name\":" : ",{\"name\":", stdout);
            print_string(c->globals[g].name);
            fputs(",\"fixture\":", stdout);
            print_string(NAME_OF(fixture_names, c->globals[g].fixture));
            fputs(",\"literal\":", stdout);
            print_string(c->globals[g].literal);
            putchar('}');
        }
        fputs("],\"sizes\":[", stdout);
        if (c->sizes != NULL) {
            for (const uint32_t *s = c->sizes; *s != 0; s++) printf("%s%u", s == c->sizes ? "" : ",", (unsigned)*s);
        }
        fputs("]}\n", stdout);
    }
}

/* FNV-1a (32 bits) over the UTF-16 code units of the UTF-8 text, and
 * their count: what guest.js's __hash and .length give for the same text. */
typedef struct {
    size_t units;
    uint32_t hash;
} text_hash;

static void hash_unit(text_hash *h, uint32_t unit) {
    h->hash ^= unit;
    h->hash *= 0x01000193u;
    h->units++;
}

static text_hash hash_utf16(const char *s, size_t n) {
    text_hash h = { 0, 0x811c9dc5u };
    for (size_t i = 0; i < n;) {
        unsigned char b = (unsigned char)s[i];
        uint32_t cp = b;
        size_t more = b >= 0xf0 ? 3 : b >= 0xe0 ? 2 : b >= 0xc0 ? 1 : 0;
        if (more > 0) cp = b & (0x3fu >> more);
        for (size_t k = 1; k <= more && i + k < n; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3fu);
        i += more + 1;
        if (cp >= 0x10000u) {
            hash_unit(&h, 0xd800u + ((cp - 0x10000u) >> 10));
            hash_unit(&h, 0xdc00u + ((cp - 0x10000u) & 0x3ffu));
        } else {
            hash_unit(&h, cp);
        }
    }
    return h;
}

/* `"key":{"len":...,"hash":"..."}` for a value as staticjs's writer
 * writes it, after a comma unless it is the first of its object. */
static bool print_written(bench_run *run, const char *key, sjs_value value, bool *first) {
    bench_text text = { 0 };
    if (!write_value(run, value, &text)) return false;
    text_hash h = hash_utf16(text.ptr, text.length);
    if (!*first) putchar(',');
    *first = false;
    print_string(key);
    printf(":{\"len\":%zu,\"hash\":\"%08x\"}", h.units, (unsigned)h.hash);
    text_free(&text);
    return true;
}

static int dump_values(const char *name, uint32_t n) {
    const bench_case *c = NULL;
    for (size_t i = 0; i < CASE_COUNT; i++) {
        if (strcmp(cases[i].name, name) == 0) c = &cases[i];
    }
    if (c == NULL) {
        fprintf(stderr, "dump: no case %s in bench.c\n", name);
        return 2;
    }
    static bench_run run;
    run.c = c;
    run.n = n;
    if (!load_text(&run, false) || !setup(&run)) {
        fprintf(stderr, "dump: %s: setup: %s\n", name, run.problem);
        return 1;
    }
    fputs("{\"case\":", stdout);
    print_string(c->name);
    printf(",\"n\":%u,\"globals\":{", (unsigned)n);
    bool ok = true, first = true;
    for (uint32_t g = 0; ok && g < run.global_count; g++) {
        ok = print_written(&run, c->globals[g].name, run.globals[g], &first);
    }
    putchar('}');
    if (ok && c->op == OP_EVAL) {
        sjs_run_params params = { run.program, run.globals, run.global_count };
        sjs_value result;
        ok = evaluate(&run, &params, &result);
        if (ok) {
            ok = print_written(&run, "result", result, &(bool){ false });
            sjs_release(run.context, result);
        }
    }
    fputs("}\n", stdout);
    if (!ok) {
        fprintf(stderr, "dump: %s: %s\n", name, run.problem);
        return 1;
    }
    return teardown(&run) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "cases") == 0) {
        dump_cases();
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "values") == 0) return dump_values(argv[2], (uint32_t)strtoul(argv[3], NULL, 10));
    fputs("usage: dump cases | dump values NAME N\n", stderr);
    return 2;
}
