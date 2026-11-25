/* canonical.c — tinyactors fmt: charts and system manifests written again
 * in one canonical layout, through ta_xml_writer (tool.h, docs/cli.md).
 * Freestanding.
 *
 * The layout is the writer's: two spaces per element level, an element, a
 * comment or a processing instruction per line, attributes in the order
 * written. Only the whitespace between elements changes. What a chart means
 * is kept byte for byte: attribute values as written (their references
 * too), and the whole content of an element whose content is a value or a
 * program (<script>, <data>, <content>, <assign>), of an element of another
 * namespace, and of an element with text or CDATA of its own. The parser
 * gives each element's content in document order (ta_xml_node.content), so
 * comments stay where they are.
 *
 * A file's canonical text is compiled, as its original is, with
 * TA_COMPILE_STRIP_DEBUG (the only difference layout makes is where each
 * expression is): the two images must be the same bytes, or the file is
 * refused and left as it is. */
#include "tool.h"

#include "base/memory.h"
#include "compile/xml.h"

static const char scxml_namespace[] = "http://www.w3.org/2005/07/scxml";

/* SCXML's elements whose content is a value or a program: kept as written. */
static const char *const value_elements[] = {"script", "data", "content", "assign"};

typedef enum canonical_image {
    CANONICAL_IMAGE_SAME = 0,         /* both compile to the same image */
    CANONICAL_IMAGE_NOT_COMPILED = 1, /* the original does not compile: nothing to compare */
    CANONICAL_IMAGE_DIFFERENT = 2     /* refused: the canonical text means something else */
} canonical_image;

static const ta_string_view image_names[] = {
    [CANONICAL_IMAGE_SAME] = TA_CLI_TEXT("same"),
    [CANONICAL_IMAGE_NOT_COMPILED] = TA_CLI_TEXT("not compiled"),
    [CANONICAL_IMAGE_DIFFERENT] = TA_CLI_TEXT("different")};

/* One file: what was read, its canonical text, and what became of it. */
typedef struct canonical_file {
    ta_string_view path;
    ta_string_view original;
    ta_string_view text; /* the canonical text; empty when refused */
    int refused;
    int changed;
    uint32_t image; /* canonical_image */
} canonical_file;

/* The diagnostics of every file, gathered for the answer, in the arena. */
typedef struct canonical_diagnostic {
    ta_diagnostic diagnostic;
    struct canonical_diagnostic *next;
} canonical_diagnostic;

/* What one invocation works with. */
typedef struct canonical_run {
    tool *t;
    canonical_diagnostic *first, **last;
    uint32_t diagnostic_count;
    int out_of_memory; /* a diagnostic could not be kept */
} canonical_run;

static int named(const char *name, const char *expected) {
    while (*name && *name == *expected) {
        name++;
        expected++;
    }
    return *name == *expected;
}

static ta_string_view view_of(const char *text) {
    uint32_t length = 0;
    while (text[length])
        length++;
    return (ta_string_view){text, length};
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_blank(ta_string_view text) {
    for (uint32_t i = 0; i < text.length; i++)
        if (!is_space(text.pointer[i])) return 0;
    return 1;
}

static void diagnose(canonical_run *run, ta_diagnostic diagnostic) {
    canonical_diagnostic *kept =
        (canonical_diagnostic *)ta_growable_arena_alloc(run->t->arena, sizeof *kept, 8);
    if (!kept) {
        run->out_of_memory = 1;
        return;
    }
    kept->diagnostic = diagnostic;
    kept->next = 0;
    *run->last = kept;
    run->last = &kept->next;
    run->diagnostic_count++;
}

static void refuse(canonical_run *run, canonical_file *file, ta_string_view message) {
    diagnose(run, (ta_diagnostic){TA_DIAGNOSTIC_ERROR, 0, 0, 0, file->path, message});
    file->refused = 1;
}

/* An element whose content is written as it is: a value or a program, an
 * element of another namespace, or one with text or CDATA of its own. */
static int keeps_its_content(const ta_xml_node *node) {
    if (node->namespace_uri[0] && !named(node->namespace_uri, scxml_namespace)) return 1;
    for (size_t i = 0; i < sizeof value_elements / sizeof value_elements[0]; i++)
        if (named(node->name, value_elements[i])) return 1;
    for (const ta_xml_content *piece = node->content; piece; piece = piece->next)
        if (piece->kind == TA_XML_CONTENT_CDATA || piece->kind == TA_XML_CONTENT_DECLARATION ||
            (piece->kind == TA_XML_CONTENT_TEXT && !is_blank(piece->markup)))
            return 1;
    return 0;
}

static ta_string_view qualified_name(tool *t, const ta_xml_node *node) {
    if (!node->prefix) return view_of(node->name);
    tool_text name;
    tool_text_init(t, &name);
    tool_text_append(&name, view_of(node->prefix));
    tool_text_append(&name, ta_string(":"));
    tool_text_append(&name, view_of(node->name));
    return tool_text_view(&name);
}

/* <!--text--> and <?target content?>, through the writer. */
static void write_comment(ta_xml_writer *writer, ta_string_view markup) {
    ta_xml_writer_comment(writer, (ta_string_view){markup.pointer + 4, markup.length - 7});
}

static void write_instruction(ta_xml_writer *writer, ta_string_view markup) {
    ta_string_view inside = {markup.pointer + 2, markup.length - 4};
    uint32_t target = 0;
    while (target < inside.length && !is_space(inside.pointer[target]))
        target++;
    uint32_t start = target, end = inside.length;
    while (start < end && is_space(inside.pointer[start]))
        start++;
    while (end > start && is_space(inside.pointer[end - 1]))
        end--;
    ta_xml_writer_instruction(writer, (ta_string_view){inside.pointer, target},
                              (ta_string_view){inside.pointer + start, end - start});
}

/* An element open: its frame, where its content goes on. */
typedef struct canonical_frame {
    const ta_xml_content *next;
    struct canonical_frame *outer;
} canonical_frame;

typedef struct canonical_writing {
    tool *t;
    ta_xml_writer *writer;
    ta_string_view source;
} canonical_writing;

/* Open `node` with its attributes. Its content written as it is, it is
 * closed at once; else its frame is pushed on *top. TA_ERROR_NO_MEMORY: the
 * frame could not be made. */
static ta_status open_element(const canonical_writing *writing, const ta_xml_node *node,
                              canonical_frame **top) {
    ta_xml_writer *writer = writing->writer;
    ta_xml_writer_open(writer, qualified_name(writing->t, node));
    for (const ta_xml_attribute *attribute = node->attributes; attribute; attribute = attribute->next)
        ta_xml_writer_attribute_markup(writer, view_of(attribute->name), attribute->markup);
    if (keeps_its_content(node)) {
        ta_xml_writer_markup(writer, (ta_string_view){writing->source.pointer + node->inner_start,
                                                      node->inner_end - node->inner_start});
        ta_xml_writer_close(writer);
        return TA_OK;
    }
    canonical_frame *frame =
        (canonical_frame *)ta_growable_arena_alloc(writing->t->arena, sizeof *frame, 8);
    if (!frame) return TA_ERROR_NO_MEMORY;
    frame->next = node->content;
    frame->outer = *top;
    *top = frame;
    return TA_OK;
}

/* The tree under `root`, in document order with a stack of frames. TA_OK,
 * or TA_ERROR_NO_MEMORY when a frame could not be made. */
static ta_status write_tree(const canonical_writing *writing, const ta_xml_node *root) {
    canonical_frame *frame = 0;
    if (open_element(writing, root, &frame) != TA_OK) return TA_ERROR_NO_MEMORY;
    while (frame) {
        const ta_xml_content *piece = frame->next;
        if (!piece) {
            ta_xml_writer_close(writing->writer);
            frame = frame->outer;
            continue;
        }
        frame->next = piece->next;
        if (piece->kind == TA_XML_CONTENT_ELEMENT) {
            if (open_element(writing, piece->element, &frame) != TA_OK) return TA_ERROR_NO_MEMORY;
        } else if (piece->kind == TA_XML_CONTENT_COMMENT) {
            write_comment(writing->writer, piece->markup);
        } else if (piece->kind == TA_XML_CONTENT_INSTRUCTION) {
            write_instruction(writing->writer, piece->markup);
        } /* else whitespace between elements: the writer lays them out */
    }
    return TA_OK;
}

/* The file's canonical text into file->text, or its refusal told. */
static void lay_out(canonical_run *run, canonical_file *file) {
    tool *t = run->t;
    ta_allocator allocator = ta_growable_arena_allocator(t->arena);
    tool_text path;
    tool_text_init(t, &path);
    tool_text_append(&path, file->path);
    tool_text_append(&path, (ta_string_view){"", 1}); /* NUL-terminated for the parser */
    ta_string_view terminated = tool_text_view(&path);
    ta_diagnostics diagnostics;
    ta_memset(&diagnostics, 0, sizeof diagnostics);
    ta_vector_init(&diagnostics.list, &allocator, sizeof(ta_diagnostic));
    diagnostics.arena = t->arena;
    int aborted = 0, out_of_memory = 0;
    ta_xml_content *document = 0;
    const ta_xml_node *root =
        terminated.pointer
            ? ta_xml_parse(t->arena, &(ta_xml_parse_request){file->original, &diagnostics, &allocator,
                                                             terminated.pointer, 0, &aborted,
                                                             &out_of_memory, &document})
            : 0;
    if (!root) {
        const ta_diagnostic *parsed = (const ta_diagnostic *)(const void *)diagnostics.list.data;
        for (uint32_t i = 0; i < diagnostics.list.length; i++) {
            ta_diagnostic kept = parsed[i];
            kept.file = file->path; /* the path as given, not the parser's copy */
            diagnose(run, kept);
        }
        if (!diagnostics.list.length) refuse(run, file, ta_string("out of memory reading the XML"));
        file->refused = 1;
        return;
    }
    ta_xml_writer *writer = 0;
    if (ta_xml_writer_new(&allocator, &writer) != TA_OK) {
        refuse(run, file, ta_string("out of memory writing the canonical text"));
        return;
    }
    canonical_writing writing = {t, writer, file->original};
    ta_status status = TA_OK;
    for (const ta_xml_content *piece = document; piece && status == TA_OK; piece = piece->next) {
        if (piece->kind == TA_XML_CONTENT_ELEMENT) status = write_tree(&writing, piece->element);
        else if (piece->kind == TA_XML_CONTENT_COMMENT) write_comment(writer, piece->markup);
        else if (piece->kind == TA_XML_CONTENT_INSTRUCTION) write_instruction(writer, piece->markup);
        else if (piece->kind == TA_XML_CONTENT_DECLARATION) status = TA_ERROR_UNSUPPORTED;
    }
    ta_string_view written = {(const char *)0, 0};
    if (status == TA_OK) status = ta_xml_writer_finish(writer, &written);
    if (status == TA_OK) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, written);
        tool_text_append(&text, ta_string("\n"));
        file->text = tool_text_view(&text);
        if (!file->text.pointer) status = TA_ERROR_NO_MEMORY;
    }
    ta_xml_writer_free(writer);
    if (status == TA_ERROR_UNSUPPORTED)
        refuse(run, file, ta_string("a document type declaration, which fmt does not keep"));
    else if (status == TA_ERROR_NO_MEMORY)
        refuse(run, file, ta_string("out of memory writing the canonical text"));
    else if (status != TA_OK) /* what XML does not allow, though the parser read it */
        refuse(run, file, ta_string("it cannot be written as XML: a comment holding '--' or ending "
                                    "with '-', or an XML declaration that is not first"));
}

/* The image `text` compiles to as the file `path`, with no positions:
 * TA_OK and its bytes, or the compile's refusal. */
static ta_status image_of(canonical_run *run, const canonical_file *file, ta_string_view text,
                          ta_bytes *out) {
    ta_source source = {file->path, text};
    uint32_t kind = tool_input_kind_of((ta_bytes){file->original.pointer, file->original.length});
    tool_compilation compilation = {kind, TA_COMPILE_STRIP_DEBUG, {&source, 1}};
    tool_image image;
    ta_memset(&image, 0, sizeof image);
    ta_status kept = tool_compile_sources(run->t, &compilation, &image);
    *out = image.bytes;
    return kept != TA_OK ? kept : image.status;
}

/* Does the canonical text mean what the original does? */
static void compare(canonical_run *run, canonical_file *file) {
    ta_bytes original, canonical;
    if (image_of(run, file, file->original, &original) != TA_OK) {
        file->image = CANONICAL_IMAGE_NOT_COMPILED;
        return;
    }
    ta_status status = image_of(run, file, file->text, &canonical);
    int same = status == TA_OK && original.length == canonical.length &&
               ta_memcmp(original.pointer, canonical.pointer, original.length) == 0;
    file->image = same ? CANONICAL_IMAGE_SAME : CANONICAL_IMAGE_DIFFERENT;
    if (!same)
        refuse(run, file, ta_string("its canonical text compiles to another image, so it is left "
                                    "as it is: a fault in fmt, please report it"));
}

/* Read, lay out and compare one file. TA_OK; TA_ERROR_INVALID when it could
 * not be read (told, t->exit set). */
static ta_status canonical_of(canonical_run *run, canonical_file *file) {
    tool *t = run->t;
    tool_read read = {file->path, t->arena};
    ta_bytes bytes;
    if (t->platform->read(t->platform->context, &read, &bytes) != TA_OK) {
        tool_text text;
        tool_text_init(t, &text);
        tool_text_append(&text, ta_string("cannot read "));
        tool_text_append(&text, file->path);
        tool_complain(t, tool_text_view(&text));
        t->exit = TOOL_EXIT_FAILURE;
        return TA_ERROR_INVALID;
    }
    file->original = (ta_string_view){(const char *)bytes.pointer, (uint32_t)bytes.length};
    uint32_t kind = tool_input_kind_of(bytes);
    if (kind != TOOL_INPUT_CHART && kind != TOOL_INPUT_MANIFEST) {
        refuse(run, file, kind == TOOL_INPUT_IMAGE
                              ? ta_string("an image: fmt takes SCXML charts and system manifests")
                          : kind == TOOL_INPUT_SNAPSHOT || kind == TOOL_INPUT_HOST_SNAPSHOT
                              ? ta_string("a snapshot, not XML: fmt takes SCXML charts and system "
                                          "manifests")
                          : kind == TOOL_INPUT_BLOB
                              ? ta_string("a blob, not XML: fmt takes SCXML charts and system manifests")
                              : ta_string("neither an SCXML chart nor a system manifest"));
        return TA_OK;
    }
    lay_out(run, file);
    if (file->refused) return TA_OK;
    file->changed = !tool_equal(file->text, file->original);
    compare(run, file);
    return TA_OK;
}

static void report(canonical_run *run, const canonical_file *files, uint32_t count) {
    tool *t = run->t;
    int writing = tool_count(t, TOOL_CANONICAL_WRITE) != 0;
    uint32_t changed = 0, refused = 0;
    for (uint32_t i = 0; i < count; i++) {
        changed += files[i].changed && !files[i].refused;
        refused += files[i].refused != 0;
    }
    ta_report_text(&t->report, ta_string("mode"),
                   writing ? ta_string("write") : tool_count(t, TOOL_CANONICAL_CHECK) ? ta_string("check")
                                                                                     : ta_string("output"));
    ta_report_number(&t->report, ta_string("files"), count);
    ta_report_number(&t->report, ta_string("changed"), changed);
    ta_report_number(&t->report, ta_string("refused"), refused);
    ta_report_list(&t->report, (ta_report_list_names){ta_string("results"), ta_string("file")});
    for (uint32_t i = 0; i < count; i++) {
        ta_report_object(&t->report, ta_string(""));
        ta_report_text(&t->report, ta_string("file"), files[i].path);
        ta_report_boolean(&t->report, ta_string("changed"), files[i].changed && !files[i].refused);
        ta_report_boolean(&t->report, ta_string("refused"), files[i].refused);
        /* refused before it was compiled: no image to name */
        int compared = files[i].text.length != 0;
        ta_report_text(&t->report, ta_string("image"), compared ? image_names[files[i].image] : ta_string(""));
        ta_report_end(&t->report);
    }
    ta_report_end(&t->report);
    tool_image gathered;
    ta_memset(&gathered, 0, sizeof gathered);
    ta_diagnostic *diagnostics =
        run->diagnostic_count
            ? (ta_diagnostic *)ta_growable_arena_alloc(t->arena, sizeof *diagnostics * run->diagnostic_count, 8)
            : 0;
    uint32_t kept = 0;
    for (const canonical_diagnostic *at = run->first; at && diagnostics; at = at->next)
        diagnostics[kept++] = at->diagnostic;
    gathered.diagnostics = diagnostics;
    gathered.diagnostic_count = kept;
    tool_report_diagnostics(t, &gathered);
}

void tool_canonical(tool *t) {
    int writing = tool_count(t, TOOL_CANONICAL_WRITE) != 0;
    int checking = tool_count(t, TOOL_CANONICAL_CHECK) != 0;
    uint32_t count = t->parsed->argument_count;
    if (writing && checking) {
        tool_complain(t, ta_string("--check writes nothing: give -w or --check, not both"));
        t->exit = TOOL_EXIT_USAGE;
        return;
    }
    if (!writing && !checking && count > 1) {
        tool_complain(t, ta_string("fmt writes one file to standard output; give -w to rewrite "
                                   "several, or --check to check them"));
        t->exit = TOOL_EXIT_USAGE;
        return;
    }
    canonical_file *files = (canonical_file *)ta_growable_arena_alloc(t->arena, sizeof *files * count, 8);
    if (!files) {
        tool_complain(t, ta_string("out of memory"));
        t->exit = TOOL_EXIT_FAILURE;
        return;
    }
    ta_memset(files, 0, sizeof *files * count);
    canonical_run run;
    ta_memset(&run, 0, sizeof run);
    run.t = t;
    run.last = &run.first;
    for (uint32_t i = 0; i < count; i++) {
        files[i].path = t->parsed->arguments[i];
        if (canonical_of(&run, &files[i]) != TA_OK) return;
    }
    uint32_t refused = 0, changed = 0;
    for (uint32_t i = 0; i < count; i++) {
        refused += files[i].refused != 0;
        changed += files[i].changed && !files[i].refused;
    }
    if (!writing && !checking && !refused) { /* one file, its canonical text the answer */
        tool_write request = {TOOL_STANDARD_OUTPUT, {0, 0}, {files[0].text.pointer, files[0].text.length}};
        if (t->platform->write(t->platform->context, &request) != TA_OK) t->exit = TOOL_EXIT_FAILURE;
        t->answered = 1;
        return;
    }
    for (uint32_t i = 0; writing && i < count; i++) {
        if (files[i].refused || !files[i].changed) continue;
        tool_write request = {0, files[i].path, {files[i].text.pointer, files[i].text.length}};
        if (t->platform->write(t->platform->context, &request) != TA_OK) {
            tool_text text;
            tool_text_init(t, &text);
            tool_text_append(&text, ta_string("cannot write "));
            tool_text_append(&text, files[i].path);
            tool_complain(t, tool_text_view(&text));
            t->exit = TOOL_EXIT_FAILURE;
            return;
        }
    }
    report(&run, files, count);
    if (run.out_of_memory) t->exit = TOOL_EXIT_FAILURE;
    else if (refused || (checking && changed)) t->exit = TOOL_EXIT_REFUSED;
}
