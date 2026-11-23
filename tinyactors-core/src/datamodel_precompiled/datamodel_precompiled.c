/* datamodel_precompiled.c — a datamodel whose expressions the application's
 * language compiled ahead of time (include/tinyactors/datamodel_precompiled.h):
 * a spike on a branch, not a decided layer.
 *
 * This layer keeps what is not the language's: payloads (CBOR blocks with a
 * reference count), each session's `_event` (a CBOR map), the captures of
 * <send>, <invoke> and <donedata> (maps of the values the language answers),
 * <foreach> (the array's value, each item assigned through the language), and
 * the framing of a blob. Everything else is one request to the language.
 * Freestanding: it allocates only through the allocator in its config.
 *
 * Spike simplifications: values the language answers are not checked
 * against the value table (a binding's codec writes them); migrate and
 * snapshots are not offered.
 */
#include <tinyactors/cbor.h>
#include <tinyactors/datamodel_precompiled.h>
#include <tinyactors/table.h>

#include "base/memory.h"

#define NIL 0xFFFFFFFFu

typedef struct pchart {
    const ta_image *image;
    uint32_t chart;  /* the image's */
    uint32_t handle; /* the language's */
} pchart;

typedef struct psession {
    pchart *chart;
    uint64_t session; /* the language's */
    uint8_t *event;   /* `_event` as CBOR, kept for serialize */
    uint32_t event_length;
    uint32_t pad;
} psession;

typedef struct ppayload {
    uint32_t references, length;
    uint8_t *bytes;
} ppayload;

typedef struct ploop {
    uint8_t *bytes; /* the array, copied at foreach_begin */
    uint32_t length;
    uint32_t pad;
} ploop;

/* what ta_datamodel_precompiled_in reads, during a request */
typedef struct pscope {
    const ta_datamodel_environment *environment;
    const pchart *chart;
} pscope;

/* a growable byte buffer */
typedef struct pbuffer {
    const ta_allocator *allocator;
    uint8_t *bytes;
    size_t length, capacity;
    int failed;
} pbuffer;

struct ta_datamodel_precompiled {
    ta_allocator allocator;
    ta_precompiled_function function;
    void *context;
    ta_datamodel_vtable vtable; /* the shared one, under this datamodel's name */
    ta_table sessions, payloads, loops;
    uint8_t *room; /* the language's answers */
    uint32_t room_size;
    uint32_t reason;               /* the last call's TA_DATAMODEL_FAILURE_* */
    char message[512];             /* and its text */
    uint32_t message_length;
    pbuffer out;                   /* the datamodel's last output, until its next call */
};

/* ---- memory and bytes ------------------------------------------------------------------ */

static void *allocate(const ta_allocator *allocator, size_t size) {
    return allocator->alloc(allocator->context, size ? size : 1, 8);
}

static void release_memory(const ta_allocator *allocator, void *pointer, size_t size) {
    if (pointer) allocator->free(allocator->context, pointer, size ? size : 1);
}

static uint8_t *copy_bytes(const ta_allocator *allocator, const void *bytes, size_t length) {
    uint8_t *copy = (uint8_t *)allocate(allocator, length);
    if (copy && length) ta_memcpy(copy, bytes, length);
    return copy;
}

static int buffer_reserve(pbuffer *b, size_t more) {
    if (b->failed) return 0;
    if (b->length + more <= b->capacity) return 1;
    size_t capacity = b->capacity ? b->capacity * 2 : 256;
    while (capacity < b->length + more) capacity *= 2;
    uint8_t *bytes = (uint8_t *)allocate(b->allocator, capacity);
    if (!bytes) return b->failed = 1, 0;
    if (b->length) ta_memcpy(bytes, b->bytes, b->length);
    release_memory(b->allocator, b->bytes, b->capacity);
    b->bytes = bytes;
    b->capacity = capacity;
    return 1;
}

static void buffer_put(pbuffer *b, const void *bytes, size_t length) {
    if (!length || !buffer_reserve(b, length)) return;
    ta_memcpy(b->bytes + b->length, bytes, length);
    b->length += length;
}

static void buffer_free(pbuffer *b) {
    release_memory(b->allocator, b->bytes, b->capacity);
    b->bytes = 0;
    b->length = b->capacity = 0;
    b->failed = 0;
}

static void put_head(pbuffer *b, unsigned major, uint64_t value) {
    uint8_t head[9];
    size_t length = 1;
    head[0] = (uint8_t)(major << 5);
    if (value < 24) head[0] |= (uint8_t)value;
    else {
        unsigned bytes = value <= 0xFF ? 1 : value <= 0xFFFF ? 2 : value <= 0xFFFFFFFFu ? 4 : 8;
        head[0] |= (uint8_t)(bytes == 1 ? 24 : bytes == 2 ? 25 : bytes == 4 ? 26 : 27);
        for (unsigned i = 0; i < bytes; i++) head[1 + i] = (uint8_t)(value >> (8 * (bytes - 1 - i)));
        length = 1 + bytes;
    }
    buffer_put(b, head, length);
}

static void put_text(pbuffer *b, ta_string_view text) {
    put_head(b, 3, text.length);
    buffer_put(b, text.pointer, text.length);
}

/* the length of the first whole item of `bytes`, 0 when it is not one */
static size_t item_length(ta_bytes bytes) {
    ta_cbor_reader reader;
    ta_cbor_item item;
    ta_cbor_reader_init(&reader, bytes);
    do
        if (ta_cbor_read(&reader, &item) != 1) return 0;
    while (reader.depth);
    return reader.at;
}

/* ---- requests -------------------------------------------------------------------------- */

static void set_message(ta_datamodel_precompiled *datamodel, const char *text) {
    uint32_t length = 0;
    while (text[length] && length < sizeof datamodel->message) length++;
    ta_memcpy(datamodel->message, text, length);
    datamodel->message_length = length;
    datamodel->reason = TA_DATAMODEL_FAILURE_EXCEPTION;
}

static ta_precompiled_request request_for(uint32_t operation, const psession *s) {
    ta_precompiled_request request;
    ta_memset(&request, 0, sizeof request);
    request.size = sizeof request;
    request.operation = operation;
    if (s) {
        request.chart = s->chart->handle;
        request.session = s->session;
    }
    return request;
}

/* one call into the language: its answer in datamodel->room */
static ta_status ask(ta_datamodel_precompiled *datamodel, ta_precompiled_request *request, const pscope *scope) {
    request->output = (ta_buffer){datamodel->room, datamodel->room_size, 0};
    request->message = (ta_buffer){(unsigned char *)datamodel->message, sizeof datamodel->message, 0};
    request->scope = scope;
    datamodel->reason = TA_DATAMODEL_FAILURE_NONE;
    datamodel->message_length = 0;
    ta_status status = datamodel->function(datamodel->context, request);
    if (status != TA_OK) {
        datamodel->reason = TA_DATAMODEL_FAILURE_EXCEPTION;
        datamodel->message_length = request->message.length < sizeof datamodel->message ? (uint32_t)request->message.length
                                                                                        : (uint32_t)sizeof datamodel->message;
        return status;
    }
    if (request->output.length > datamodel->room_size) return set_message(datamodel, "an answer larger than its room"), TA_ERROR_NO_MEMORY;
    return TA_OK;
}

static psession *session_of(const ta_datamodel_call *call) {
    return (psession *)ta_table_get(&((ta_datamodel_precompiled *)call->datamodel)->sessions, call->instance);
}

/* a request about one session's expression, with In() available */
static ta_status ask_session(const ta_datamodel_call *call, ta_precompiled_request *request) {
    psession *s = session_of(call);
    if (!s) return TA_ERROR_INVALID;
    request->chart = s->chart->handle;
    request->session = s->session;
    pscope scope = {call->environment, s->chart};
    return ask((ta_datamodel_precompiled *)call->datamodel, request, &scope);
}

int ta_datamodel_precompiled_in(const ta_precompiled_request *request, ta_string_view state) {
    const pscope *scope = (const pscope *)request->scope;
    if (!scope || !scope->environment) return 0;
    uint32_t index = ta_image_state_by_name(scope->chart->image, scope->chart->chart, state);
    return index != NIL && ta_datamodel_environment_in(scope->environment, index);
}

/* ---- the fingerprint ------------------------------------------------------------------- */

static uint64_t fnv(uint64_t hash, const void *bytes, size_t length) {
    const uint8_t *p = (const uint8_t *)bytes;
    for (size_t i = 0; i < length; i++) hash = (hash ^ p[i]) * 0x100000001b3ull;
    return hash;
}

uint64_t ta_datamodel_precompiled_fingerprint(const ta_image *image, uint32_t chart) {
    uint64_t hash = 0xcbf29ce484222325ull;
    uint32_t count = ta_image_expression_count(image, chart);
    for (uint32_t i = 0; i < count; i++) {
        ta_expression_info info;
        if (ta_image_expression(image, (ta_chart_item){chart, i}, &info) != TA_OK) continue;
        uint8_t kind = (uint8_t)info.kind;
        hash = fnv(hash, &kind, 1);
        hash = fnv(hash, info.source.pointer, info.source.length);
        hash = fnv(hash, "", 1);
    }
    uint32_t data = ta_image_data_count(image, chart);
    for (uint32_t i = 0; i < data; i++) {
        ta_data_info info;
        if (ta_image_data(image, (ta_chart_item){chart, i}, &info) != TA_OK) continue;
        hash = fnv(hash, info.id.pointer, info.id.length);
        hash = fnv(hash, "", 1);
    }
    return hash;
}

/* ---- payloads -------------------------------------------------------------------------- */

static ta_payload payload_new(ta_datamodel_precompiled *datamodel, const void *bytes, size_t length) {
    uint64_t handle;
    uint8_t *copy = copy_bytes(&datamodel->allocator, bytes, length);
    if (!copy) return 0;
    ppayload *row = (ppayload *)ta_table_add(&datamodel->payloads, &handle);
    if (!row) return release_memory(&datamodel->allocator, copy, length), 0;
    row->references = 1;
    row->length = (uint32_t)length;
    row->bytes = copy;
    return handle;
}

static void p_retain(void *d, ta_payload p) {
    ppayload *row = (ppayload *)ta_table_get(&((ta_datamodel_precompiled *)d)->payloads, p);
    if (row) row->references++;
}

static void p_release(void *d, ta_payload p) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)d;
    ppayload *row = (ppayload *)ta_table_get(&datamodel->payloads, p);
    if (!row || --row->references) return;
    release_memory(&datamodel->allocator, row->bytes, row->length);
    ta_table_remove(&datamodel->payloads, p);
}

/* ---- charts and sessions --------------------------------------------------------------- */

static ta_status p_attach(void *d, const ta_datamodel_chart *chart, void **context) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)d;
    pchart *c = (pchart *)allocate(&datamodel->allocator, sizeof *c);
    if (!c) return TA_ERROR_NO_MEMORY;
    c->image = chart->image;
    c->chart = chart->chart;
    ta_string_view name = ta_image_chart_name(chart->image, chart->chart);
    ta_precompiled_request request = request_for(TA_PRECOMPILED_ATTACH, 0);
    request.input = (ta_bytes){name.pointer, name.length};
    request.session = ta_datamodel_precompiled_fingerprint(chart->image, chart->chart);
    ta_status status = ask(datamodel, &request, 0);
    if (status != TA_OK) return release_memory(&datamodel->allocator, c, sizeof *c), status;
    c->handle = request.number;
    *context = c;
    return TA_OK;
}

static void p_detach(void *d, void *context) {
    release_memory(&((ta_datamodel_precompiled *)d)->allocator, context, sizeof(pchart));
}

/* [sessionid, name, {type: location}, params] */
static void start_bytes(ta_datamodel_precompiled *datamodel, const ta_datamodel_start *start, pbuffer *b) {
    const ta_datamodel_system_variables *v = start->system_variables;
    put_head(b, 4, 4);
    put_text(b, v ? v->sessionid : (ta_string_view){"", 0});
    put_text(b, v ? v->name : (ta_string_view){"", 0});
    put_head(b, 5, v ? v->io_processor_count : 0);
    for (uint32_t i = 0; v && i < v->io_processor_count; i++) {
        put_text(b, v->io_processors[i].type);
        put_text(b, v->io_processors[i].location);
    }
    ppayload *row = start->init ? (ppayload *)ta_table_get(&datamodel->payloads, start->init) : 0;
    if (row) buffer_put(b, row->bytes, row->length);
    else put_head(b, 7, TA_CBOR_UNDEFINED);
}

static ta_status session_start(ta_datamodel_precompiled *datamodel, const ta_datamodel_start *start, uint32_t operation,
                               uint64_t *out) {
    pchart *c = (pchart *)start->chart_context;
    pbuffer input = {&datamodel->allocator, 0, 0, 0, 0};
    start_bytes(datamodel, start, &input);
    if (operation == TA_PRECOMPILED_DESERIALIZE) buffer_put(&input, start->state.pointer, start->state.length);
    if (input.failed) return buffer_free(&input), TA_ERROR_NO_MEMORY;
    psession s = {c, 0, 0, 0, 0};
    ta_precompiled_request request = request_for(operation, &s);
    request.input = (ta_bytes){input.bytes, input.length};
    pscope scope = {0, c};
    ta_status status = ask(datamodel, &request, &scope);
    buffer_free(&input);
    if (status != TA_OK) return status;
    psession *row = (psession *)ta_table_add(&datamodel->sessions, out);
    if (!row) {
        s.session = request.session;
        ta_precompiled_request destroy = request_for(TA_PRECOMPILED_DESTROY, &s);
        ask(datamodel, &destroy, 0);
        return TA_ERROR_NO_MEMORY;
    }
    *row = (psession){c, request.session, 0, 0, 0};
    return TA_OK;
}

/* the init payload: the layer gave its bytes in CREATE's input; core releases
 * it only when no create runs (core.c), so create releases it */
static ta_status p_create(void *d, const ta_datamodel_start *start, uint64_t *out) {
    ta_status status = session_start((ta_datamodel_precompiled *)d, start, TA_PRECOMPILED_CREATE, out);
    if (start->init) p_release(d, start->init);
    return status;
}

static void p_destroy(const ta_datamodel_call *call) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)call->datamodel;
    psession *s = session_of(call);
    if (!s) return;
    ta_precompiled_request request = request_for(TA_PRECOMPILED_DESTROY, s);
    ask(datamodel, &request, 0);
    release_memory(&datamodel->allocator, s->event, s->event_length);
    ta_table_remove(&datamodel->sessions, call->instance);
}

/* ---- the calls a step makes ----------------------------------------------------------- */

static ta_status p_init_data(const ta_datamodel_call *call, uint32_t declaration) {
    ta_precompiled_request request = request_for(TA_PRECOMPILED_INIT_DATA, 0);
    request.number = declaration;
    return ask_session(call, &request);
}

static ta_status p_bind_event(const ta_datamodel_call *call, const ta_event_view *view) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)call->datamodel;
    psession *s = session_of(call);
    if (!s) return TA_ERROR_INVALID;
    static const char *const types[] = {"platform", "internal", "external"};
    pbuffer b = {&datamodel->allocator, 0, 0, 0, 0};
    put_head(&b, 5, 8);
    put_text(&b, ta_string("name"));
    put_text(&b, view->name);
    put_text(&b, ta_string("type"));
    put_text(&b, ta_string(types[view->type % 3]));
    put_text(&b, ta_string("sendid"));
    put_text(&b, view->sendid);
    put_text(&b, ta_string("origin"));
    put_text(&b, view->origin);
    put_text(&b, ta_string("origintype"));
    put_text(&b, view->origintype);
    put_text(&b, ta_string("invokeid"));
    put_text(&b, view->invokeid);
    put_text(&b, ta_string("raw"));
    put_text(&b, view->raw);
    put_text(&b, ta_string("data"));
    ppayload *row = view->data ? (ppayload *)ta_table_get(&datamodel->payloads, view->data) : 0;
    if (row) buffer_put(&b, row->bytes, row->length);
    else put_head(&b, 7, TA_CBOR_UNDEFINED);
    if (b.failed) return buffer_free(&b), TA_ERROR_NO_MEMORY;
    release_memory(&datamodel->allocator, s->event, s->event_length);
    s->event = copy_bytes(&datamodel->allocator, b.bytes, b.length);
    s->event_length = s->event ? (uint32_t)b.length : 0;
    buffer_free(&b);
    if (!s->event) return TA_ERROR_NO_MEMORY;
    ta_precompiled_request request = request_for(TA_PRECOMPILED_BIND_EVENT, 0);
    request.input = (ta_bytes){s->event, s->event_length};
    return ask_session(call, &request);
}

static void p_detach_event(const ta_datamodel_call *call) { (void)call; /* `_event` is the language's copy */ }

static ta_status p_condition(const ta_datamodel_call *call, ta_expression x, int *out) {
    ta_precompiled_request request = request_for(TA_PRECOMPILED_CONDITION, 0);
    request.expression = x;
    ta_status status = ask_session(call, &request);
    *out = status == TA_OK && request.number != 0;
    return status;
}

/* the answer's bytes, kept until the datamodel's next call */
static ta_status keep_output(ta_datamodel_precompiled *datamodel, const ta_precompiled_request *request, ta_string_view *out) {
    datamodel->out.length = 0;
    buffer_put(&datamodel->out, datamodel->room, request->output.length);
    if (datamodel->out.failed) return TA_ERROR_NO_MEMORY;
    *out = (ta_string_view){(const char *)datamodel->out.bytes, (uint32_t)request->output.length};
    return TA_OK;
}

static ta_status p_string(const ta_datamodel_call *call, ta_expression x, ta_string_view *out) {
    ta_precompiled_request request = request_for(TA_PRECOMPILED_STRING, 0);
    request.expression = x;
    ta_status status = ask_session(call, &request);
    return status == TA_OK ? keep_output((ta_datamodel_precompiled *)call->datamodel, &request, out) : status;
}

static ta_status p_assign(const ta_datamodel_call *call, ta_expression location, ta_expression value) {
    ta_precompiled_request request = request_for(TA_PRECOMPILED_ASSIGN, 0);
    request.expression = location;
    request.number = value;
    return ask_session(call, &request);
}

static ta_status assign_value(const ta_datamodel_call *call, ta_expression location, ta_bytes value) {
    ta_precompiled_request request = request_for(TA_PRECOMPILED_ASSIGN_VALUE, 0);
    request.expression = location;
    request.input = value;
    return ask_session(call, &request);
}

static ta_status p_assign_string(const ta_datamodel_call *call, ta_expression location, ta_string_view value) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)call->datamodel;
    pbuffer b = {&datamodel->allocator, 0, 0, 0, 0};
    put_text(&b, value);
    ta_status status = b.failed ? TA_ERROR_NO_MEMORY : assign_value(call, location, (ta_bytes){b.bytes, b.length});
    buffer_free(&b);
    return status;
}

static ta_status p_script(const ta_datamodel_call *call, ta_expression x) {
    ta_precompiled_request request = request_for(TA_PRECOMPILED_SCRIPT, 0);
    request.expression = x;
    return ask_session(call, &request);
}

static ta_status p_log(const ta_datamodel_call *call, ta_expression x, ta_string_view *out) {
    return p_string(call, x, out);
}

/* ---- <foreach>: the array's value, each item assigned through the language ------------ */

static ta_status p_foreach_begin(const ta_datamodel_call *call, ta_expression array, ta_datamodel_loop *loop) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)call->datamodel;
    ta_precompiled_request request = request_for(TA_PRECOMPILED_VALUE, 0);
    request.expression = array;
    ta_status status = ask_session(call, &request);
    if (status != TA_OK) return status;
    ta_cbor_reader reader;
    ta_cbor_item item;
    ta_cbor_reader_init(&reader, (ta_bytes){datamodel->room, request.output.length});
    if (ta_cbor_read(&reader, &item) != 1 || item.kind != TA_CBOR_ARRAY)
        return set_message(datamodel, "<foreach> needs an array"), TA_ERROR_INVALID;
    ploop *row = (ploop *)ta_table_add(&datamodel->loops, &loop->iter);
    if (!row) return TA_ERROR_NO_MEMORY;
    row->bytes = copy_bytes(&datamodel->allocator, datamodel->room, request.output.length);
    row->length = (uint32_t)request.output.length;
    if (!row->bytes) return ta_table_remove(&datamodel->loops, loop->iter), TA_ERROR_NO_MEMORY;
    loop->n = item.count;
    return TA_OK;
}

static ta_status p_foreach_item(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)call->datamodel;
    ploop *row = (ploop *)ta_table_get(&datamodel->loops, loop->iter);
    if (!row) return TA_ERROR_INVALID;
    /* the item: walked to from the array's head (a spike: no offsets kept) */
    size_t at = 1;
    ta_cbor_reader reader;
    ta_cbor_item head;
    ta_cbor_reader_init(&reader, (ta_bytes){row->bytes, row->length});
    ta_cbor_read(&reader, &head);
    at = reader.at;
    for (uint32_t i = 0; i < loop->i; i++) at += item_length((ta_bytes){row->bytes + at, row->length - at});
    size_t length = item_length((ta_bytes){row->bytes + at, row->length - at});
    if (!length) return TA_ERROR_INVALID;
    ta_status status = assign_value(call, loop->item, (ta_bytes){row->bytes + at, length});
    if (status != TA_OK || loop->index == TA_NIL) return status;
    uint8_t index[9];
    pbuffer b = {&datamodel->allocator, index, 0, sizeof index, 0};
    put_head(&b, 0, loop->i);
    return assign_value(call, loop->index, (ta_bytes){b.bytes, b.length});
}

static void p_foreach_end(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)call->datamodel;
    ploop *row = (ploop *)ta_table_get(&datamodel->loops, loop->iter);
    if (!row) return;
    release_memory(&datamodel->allocator, row->bytes, row->length);
    ta_table_remove(&datamodel->loops, loop->iter);
}

/* ---- captures -------------------------------------------------------------------------- */

static ta_status value_into(const ta_datamodel_call *call, ta_expression x, pbuffer *b) {
    ta_precompiled_request request = request_for(TA_PRECOMPILED_VALUE, 0);
    request.expression = x;
    ta_status status = ask_session(call, &request);
    if (status == TA_OK) buffer_put(b, ((ta_datamodel_precompiled *)call->datamodel)->room, request.output.length);
    return status;
}

static ta_status p_capture(const ta_datamodel_call *call, const ta_datamodel_capture *spec, ta_payload *out) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)call->datamodel;
    psession *s = session_of(call);
    if (!s) return TA_ERROR_INVALID;
    pbuffer b = {&datamodel->allocator, 0, 0, 0, 0};
    ta_status status = TA_OK;
    if (spec->content != TA_NIL) status = value_into(call, spec->content, &b);
    else {
        /* a map of names and values; a name given twice keeps both (the
         * language's own map type decides when it reads it) */
        put_head(&b, 5, spec->namelist_count + spec->param_count);
        for (uint32_t i = 0; status == TA_OK && i < spec->namelist_count; i++) {
            ta_expression_info info;
            ta_image_expression(s->chart->image, (ta_chart_item){s->chart->chart, spec->namelist[i]}, &info);
            put_text(&b, info.source);
            status = value_into(call, spec->namelist[i], &b);
        }
        for (uint32_t i = 0; status == TA_OK && i < spec->param_count; i++) {
            put_text(&b, ta_image_string(s->chart->image, spec->params[2 * i]));
            status = value_into(call, spec->params[2 * i + 1], &b);
        }
    }
    if (status == TA_OK && b.failed) status = TA_ERROR_NO_MEMORY;
    if (status == TA_OK) {
        *out = payload_new(datamodel, b.bytes, b.length);
        if (!*out) status = TA_ERROR_NO_MEMORY;
    }
    buffer_free(&b);
    return status;
}

static ta_status p_custom(const ta_datamodel_call *call, uint32_t definition) {
    (void)call;
    (void)definition;
    return TA_ERROR_UNSUPPORTED;
}

/* ---- blobs: {"vars": <the language's map>, "event": <_event> | null} -------------------- */

static ta_status p_serialize(const ta_datamodel_call *call, ta_bytes *out) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)call->datamodel;
    psession *s = session_of(call);
    if (!s) return TA_ERROR_INVALID;
    ta_precompiled_request request = request_for(TA_PRECOMPILED_SERIALIZE, 0);
    ta_status status = ask_session(call, &request);
    if (status != TA_OK) return status;
    pbuffer *b = &datamodel->out;
    b->length = 0;
    put_head(b, 5, 2);
    put_text(b, ta_string("vars"));
    buffer_put(b, datamodel->room, request.output.length);
    put_text(b, ta_string("event"));
    if (s->event) buffer_put(b, s->event, s->event_length);
    else put_head(b, 7, TA_CBOR_NULL);
    if (b->failed) return TA_ERROR_NO_MEMORY;
    *out = (ta_bytes){b->bytes, b->length};
    return TA_OK;
}

static ta_status p_deserialize(void *d, const ta_datamodel_start *start, uint64_t *out) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)d;
    /* the blob is a map of two: "vars", then "event" */
    ta_bytes blob = start->state;
    const uint8_t *p = (const uint8_t *)blob.pointer;
    if (blob.length < 7 || p[0] != 0xA2 || p[1] != 0x64 || ta_memcmp(p + 2, "vars", 4)) return TA_ERROR_INVALID;
    size_t vars = item_length((ta_bytes){p + 6, blob.length - 6});
    size_t at = 6 + vars;
    if (!vars || blob.length < at + 6 || ta_memcmp(p + at, "\x65" "event", 6)) return TA_ERROR_INVALID;
    ta_datamodel_start variables = *start;
    variables.state = (ta_bytes){p + 6, vars};
    ta_status status = session_start(datamodel, &variables, TA_PRECOMPILED_DESERIALIZE, out);
    if (status != TA_OK) return status;
    ta_bytes event = {p + at + 6, blob.length - at - 6};
    if (event.length && p[at + 6] != 0xF6) {
        ta_datamodel_call call = {datamodel, *out, 0};
        psession *s = session_of(&call);
        s->event = copy_bytes(&datamodel->allocator, event.pointer, event.length);
        s->event_length = s->event ? (uint32_t)event.length : 0;
        ta_precompiled_request request = request_for(TA_PRECOMPILED_BIND_EVENT, 0);
        request.input = (ta_bytes){s->event, s->event_length};
        status = ask_session(&call, &request);
    }
    return status;
}

/* ---- values ---------------------------------------------------------------------------- */

static ta_status p_to_wire(void *d, ta_payload p, ta_bytes *out) {
    ppayload *row = (ppayload *)ta_table_get(&((ta_datamodel_precompiled *)d)->payloads, p);
    if (!row) return TA_ERROR_INVALID;
    *out = (ta_bytes){row->bytes, row->length};
    return TA_OK;
}

static ta_status p_from_wire(void *d, ta_bytes cbor, ta_payload *out) {
    if (!cbor.length || item_length(cbor) != cbor.length) return TA_ERROR_INVALID;
    *out = payload_new((ta_datamodel_precompiled *)d, cbor.pointer, cbor.length);
    return *out ? TA_OK : TA_ERROR_NO_MEMORY;
}

static ta_status p_variable(const ta_datamodel_call *call, ta_string_view name, ta_payload *out) {
    ta_precompiled_request request = request_for(TA_PRECOMPILED_VARIABLE, 0);
    request.input = (ta_bytes){name.pointer, name.length};
    ta_status status = ask_session(call, &request);
    if (status != TA_OK) return status;
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)call->datamodel;
    *out = payload_new(datamodel, datamodel->room, request.output.length);
    return *out ? TA_OK : TA_ERROR_NO_MEMORY;
}

static ta_status p_failure(const ta_datamodel_call *call, ta_datamodel_failure *out) {
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)call->datamodel;
    out->reason = datamodel->reason;
    out->message = (ta_string_view){datamodel->message, datamodel->message_length};
    return TA_OK;
}

static const ta_datamodel_vtable precompiled_vtable = {
    sizeof(ta_datamodel_vtable),
    TA_DATAMODEL_EVENT_DETACHABLE,
    "precompiled",
    p_attach,
    p_detach,
    p_create,
    p_destroy,
    p_init_data,
    p_bind_event,
    p_detach_event,
    p_condition,
    p_string,
    p_assign,
    p_assign_string,
    p_script,
    p_log,
    p_foreach_begin,
    p_foreach_item,
    p_foreach_end,
    p_capture,
    p_retain,
    p_release,
    p_custom,
    p_serialize,
    p_deserialize,
    0, /* migrate: not in the spike */
    p_to_wire,
    p_from_wire,
    p_to_wire,   /* payload_serialize: the bytes as they are */
    p_from_wire, /* payload_deserialize */
    p_variable,
    p_failure,
};

/* ---- the datamodel --------------------------------------------------------------------- */

ta_status ta_datamodel_precompiled_new(const ta_datamodel_precompiled_config *config, ta_datamodel_precompiled **out) {
    if (!config->function || !config->name) return TA_ERROR_INVALID;
    ta_datamodel_precompiled *datamodel = (ta_datamodel_precompiled *)allocate(&config->allocator, sizeof *datamodel);
    if (!datamodel) return TA_ERROR_NO_MEMORY;
    ta_memset(datamodel, 0, sizeof *datamodel);
    datamodel->allocator = config->allocator;
    datamodel->function = config->function;
    datamodel->context = config->context;
    datamodel->vtable = precompiled_vtable;
    datamodel->vtable.name = config->name;
    datamodel->room_size = config->max_value_bytes ? config->max_value_bytes : 64 * 1024;
    datamodel->room = (uint8_t *)allocate(&datamodel->allocator, datamodel->room_size);
    if (!datamodel->room) return release_memory(&config->allocator, datamodel, sizeof *datamodel), TA_ERROR_NO_MEMORY;
    datamodel->out = (pbuffer){&datamodel->allocator, 0, 0, 0, 0};
    ta_table_config sessions = {sizeof sessions, sizeof(psession), &datamodel->allocator};
    ta_table_config payloads = {sizeof payloads, sizeof(ppayload), &datamodel->allocator};
    ta_table_config loops = {sizeof loops, sizeof(ploop), &datamodel->allocator};
    ta_table_init(&datamodel->sessions, &sessions);
    ta_table_init(&datamodel->payloads, &payloads);
    ta_table_init(&datamodel->loops, &loops);
    *out = datamodel;
    return TA_OK;
}

ta_datamodel_binding ta_datamodel_precompiled_binding(ta_datamodel_precompiled *datamodel) {
    ta_datamodel_binding binding = {&datamodel->vtable, datamodel};
    return binding;
}

void ta_datamodel_precompiled_free(ta_datamodel_precompiled *datamodel) {
    if (!datamodel) return;
    for (uint32_t slot = 0; slot < datamodel->sessions.capacity; slot++) {
        psession *s = (psession *)ta_table_at(&datamodel->sessions, slot);
        if (s) release_memory(&datamodel->allocator, s->event, s->event_length);
    }
    for (uint32_t slot = 0; slot < datamodel->payloads.capacity; slot++) {
        ppayload *p = (ppayload *)ta_table_at(&datamodel->payloads, slot);
        if (p) release_memory(&datamodel->allocator, p->bytes, p->length);
    }
    for (uint32_t slot = 0; slot < datamodel->loops.capacity; slot++) {
        ploop *l = (ploop *)ta_table_at(&datamodel->loops, slot);
        if (l) release_memory(&datamodel->allocator, l->bytes, l->length);
    }
    ta_table_free(&datamodel->sessions);
    ta_table_free(&datamodel->payloads);
    ta_table_free(&datamodel->loops);
    buffer_free(&datamodel->out);
    release_memory(&datamodel->allocator, datamodel->room, datamodel->room_size);
    ta_allocator allocator = datamodel->allocator;
    release_memory(&allocator, datamodel, sizeof *datamodel);
}
