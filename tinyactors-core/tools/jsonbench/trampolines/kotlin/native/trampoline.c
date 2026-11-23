/* The native side of the Kotlin spike: a JNI library whose bench() loops
 * over a C trampoline with the json datamodel's function signature. The trampoline's
 * context is a slot naming an index into the table of Kotlin functions;
 * it calls the function through JNI and re-emits the CBOR item it returns
 * into call->result, walking it with ta_cbor_reader and writing each item
 * with ta_cbor_writer (the writer has no raw-bytes call), which also
 * checks what Kotlin wrote.
 *
 * The "bytes" modes call a second trampoline with the driver's own call
 * struct (ta_kotlin_bytes_call: the result is a ta_buffer the driver lends,
 * a 64 KB room), since the header's ABI is changing in core: "bytes"
 * copies the returned byte[] into the room with GetByteArrayRegion and
 * sets its length; "bytes-direct" has Kotlin put its result into a direct
 * ByteBuffer over the room and return the length; "bytes+check" is
 * "bytes" followed by a check pass over the result in C.
 *
 * Both call structs are the driver's own, not datamodel_json.h's, so the
 * spike builds while that header's ABI changes: ta_kotlin_call is the ABI
 * the "array" and "buffer" rows were measured with (the result written
 * into a ta_cbor_writer), ta_kotlin_bytes_call the lent-buffer one. */
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tinyactors/cbor.h>
#include <tinyactors/types.h>

#define TA_KOTLIN_FUNCTIONS 8
#define TA_KOTLIN_STAGING 4096
#define TA_KOTLIN_ROOM (64u * 1024u)

enum {
    TA_KOTLIN_MODE_ARRAY = 0,        /* a reused byte[] in, the result re-emitted */
    TA_KOTLIN_MODE_BUFFER = 1,       /* a direct ByteBuffer in, the result re-emitted */
    TA_KOTLIN_MODE_BYTES = 2,        /* a reused byte[] in, the result's bytes copied to the room */
    TA_KOTLIN_MODE_BYTES_DIRECT = 3, /* a reused byte[] in, Kotlin puts the result in the room */
    TA_KOTLIN_MODE_BYTES_CHECK = 4   /* "bytes", then check() over the result */
};

/* The driver's own call for the re-emit modes (the json datamodel's
 * ta_kotlin_call before its result became a ta_buffer): write exactly one
 * item into result. */
typedef struct ta_kotlin_call {
    uint32_t size;
    uint32_t argument_count;
    void *context;
    ta_bytes arguments;
    ta_cbor_writer *result;
    ta_string_view message;
} ta_kotlin_call;

typedef ta_status (*ta_kotlin_writer_function)(ta_kotlin_call *call);

/* The driver's own call for the bytes modes: the result is encoded bytes
 * in a buffer the driver lends (the call writes from its start and sets
 * its length; TA_ERROR_NO_MEMORY: length is the room it needs). */
typedef struct ta_kotlin_bytes_call {
    uint32_t size;
    uint32_t argument_count;
    void *context;
    ta_bytes arguments;
    ta_buffer *result;
    ta_string_view message;
} ta_kotlin_bytes_call;

typedef ta_status (*ta_kotlin_bytes_function)(ta_kotlin_bytes_call *call);

typedef struct ta_kotlin_table ta_kotlin_table;

typedef struct ta_kotlin_slot {
    ta_kotlin_table *table;
    uint32_t index;
} ta_kotlin_slot;

struct ta_kotlin_table {
    JNIEnv *jni;     /* the calling thread's, set by bench() */
    int mode;        /* 0: a reused byte[]; 1: a direct ByteBuffer */
    uint32_t count;
    jobject functions[TA_KOTLIN_FUNCTIONS]; /* global references */
    ta_kotlin_slot slots[TA_KOTLIN_FUNCTIONS];
    jmethodID call;  /* JsonFunction.call([B)[B */
    jbyteArray arguments; /* the reused array, a global reference */
    jsize arguments_length;
    jclass bridge;   /* Bridge, a global reference */
    jmethodID call_buffer;
    unsigned char *staging; /* the direct buffer's memory */
    jmethodID call_into;    /* Bridge.callInto(JsonFunction, [B)I */
    jmethodID set_room;     /* Bridge.setRoom(ByteBuffer) */
    unsigned char *room;    /* the room the driver lends a bytes call, TA_KOTLIN_ROOM */
    jobject room_buffer;    /* a direct ByteBuffer over it, a global reference */
    void *room_wrapped;     /* the memory room_buffer wraps */
};

/* one table per process: a spike (a binding keeps it per environment) */
static ta_kotlin_table ta_kotlin_state;

/* ---- re-emitting one CBOR item ------------------------------------------------------------- */

static ta_status ta_kotlin_copy_item(ta_cbor_writer *w, ta_bytes item) {
    ta_cbor_reader r;
    ta_cbor_item it;
    int32_t got;
    ta_cbor_reader_init(&r, item);
    while ((got = ta_cbor_read(&r, &it)) == 1) {
        switch (it.kind) {
        case TA_CBOR_UINT: ta_cbor_write_uint(w, it.value); break;
        case TA_CBOR_NEGINT: ta_cbor_write_negative(w, it.value); break;
        case TA_CBOR_TEXT:
            ta_cbor_write_text(w, (ta_string_view){it.bytes.pointer, (uint32_t)it.bytes.length});
            break;
        case TA_CBOR_BYTES: ta_cbor_write_bytes(w, it.bytes); break;
        case TA_CBOR_ARRAY: ta_cbor_write_array(w, it.count); break;
        case TA_CBOR_MAP: ta_cbor_write_map(w, it.count); break;
        case TA_CBOR_TAG: ta_cbor_write_tag(w, it.value); break;
        case TA_CBOR_SIMPLE: ta_cbor_write_simple(w, (uint32_t)it.value); break;
        case TA_CBOR_FLOAT: {
            double d;
            memcpy(&d, &it.value, 8);
            ta_cbor_write_float(w, d);
            break;
        }
        default: return TA_ERROR_INVALID;
        }
    }
    return got == 0 ? TA_OK : (ta_status)got;
}

/* ---- the trampoline ------------------------------------------------------------------------ */

/* the arguments into the reused byte[] (mode 0's way) */
static ta_status ta_kotlin_arguments_array(ta_kotlin_table *t, ta_bytes arguments) {
    JNIEnv *jni = t->jni;
    jsize length = (jsize)arguments.length;
    if (length != t->arguments_length) { /* reused while the length holds */
        jbyteArray fresh = (*jni)->NewByteArray(jni, length);
        if (!fresh) return TA_ERROR_NO_MEMORY;
        if (t->arguments) (*jni)->DeleteGlobalRef(jni, t->arguments);
        t->arguments = (*jni)->NewGlobalRef(jni, fresh);
        (*jni)->DeleteLocalRef(jni, fresh);
        t->arguments_length = length;
    }
    (*jni)->SetByteArrayRegion(jni, t->arguments, 0, length, arguments.pointer);
    return TA_OK;
}

static ta_status ta_kotlin_trampoline(ta_kotlin_call *call) {
    ta_kotlin_slot *slot = call->context;
    ta_kotlin_table *t = slot->table;
    JNIEnv *jni = t->jni;
    jsize length = (jsize)call->arguments.length;
    jobject function = t->functions[slot->index];
    jbyteArray result;
    if (t->mode == TA_KOTLIN_MODE_ARRAY) {
        ta_status s = ta_kotlin_arguments_array(t, call->arguments);
        if (s != TA_OK) return s;
        result = (*jni)->CallObjectMethod(jni, function, t->call, t->arguments);
    } else {
        if (length > TA_KOTLIN_STAGING) return TA_ERROR_NO_MEMORY;
        memcpy(t->staging, call->arguments.pointer, (size_t)length);
        result = (*jni)->CallStaticObjectMethod(jni, t->bridge, t->call_buffer, function, length);
    }
    if ((*jni)->ExceptionCheck(jni)) {
        (*jni)->ExceptionClear(jni); /* a binding would keep its message */
        call->message = ta_string("the Kotlin function threw");
        return TA_ERROR_INVALID;
    }
    jsize n = (*jni)->GetArrayLength(jni, result);
    unsigned char room[1024];
    ta_status status;
    if ((size_t)n <= sizeof room) {
        (*jni)->GetByteArrayRegion(jni, result, 0, n, (jbyte *)room);
        status = ta_kotlin_copy_item(call->result, (ta_bytes){room, (size_t)n});
    } else {
        jbyte *p = (*jni)->GetPrimitiveArrayCritical(jni, result, NULL);
        status = ta_kotlin_copy_item(call->result, (ta_bytes){p, (size_t)n});
        (*jni)->ReleasePrimitiveArrayCritical(jni, result, p, JNI_ABORT);
    }
    (*jni)->DeleteLocalRef(jni, result);
    return status;
}

/* ---- the bytes trampolines ----------------------------------------------------------------- */

static ta_status ta_kotlin_threw(JNIEnv *jni, ta_string_view *message) {
    if (!(*jni)->ExceptionCheck(jni)) return TA_OK;
    (*jni)->ExceptionClear(jni);
    *message = ta_string("the Kotlin function threw");
    return TA_ERROR_INVALID;
}

/* "bytes": the returned byte[] copied into the lent buffer, no walk */
static ta_status ta_kotlin_trampoline_bytes(ta_kotlin_bytes_call *call) {
    ta_kotlin_slot *slot = call->context;
    ta_kotlin_table *t = slot->table;
    JNIEnv *jni = t->jni;
    ta_status status = ta_kotlin_arguments_array(t, call->arguments);
    if (status != TA_OK) return status;
    jbyteArray result = (*jni)->CallObjectMethod(jni, t->functions[slot->index], t->call, t->arguments);
    status = ta_kotlin_threw(jni, &call->message);
    if (status != TA_OK) return status;
    jsize n = (*jni)->GetArrayLength(jni, result);
    call->result->length = (size_t)n;
    if ((size_t)n > call->result->capacity) {
        status = TA_ERROR_NO_MEMORY;
    } else {
        (*jni)->GetByteArrayRegion(jni, result, 0, n, (jbyte *)call->result->pointer);
    }
    (*jni)->DeleteLocalRef(jni, result);
    return status;
}

/* "bytes-direct": Kotlin puts the result into a direct ByteBuffer over the
 * lent room and returns its length (or minus the length it needs). The
 * ByteBuffer is made once over the driver's room; a room elsewhere gets one
 * of its own for the call. */
static ta_status ta_kotlin_trampoline_bytes_direct(ta_kotlin_bytes_call *call) {
    ta_kotlin_slot *slot = call->context;
    ta_kotlin_table *t = slot->table;
    JNIEnv *jni = t->jni;
    ta_status status = ta_kotlin_arguments_array(t, call->arguments);
    if (status != TA_OK) return status;
    if (call->result->pointer != t->room_wrapped || call->result->capacity != TA_KOTLIN_ROOM) {
        jobject buffer = (*jni)->NewDirectByteBuffer(jni, call->result->pointer, (jlong)call->result->capacity);
        if (!buffer) return TA_ERROR_NO_MEMORY;
        (*jni)->CallStaticVoidMethod(jni, t->bridge, t->set_room, buffer);
        if (t->room_buffer) (*jni)->DeleteGlobalRef(jni, t->room_buffer);
        t->room_buffer = (*jni)->NewGlobalRef(jni, buffer);
        (*jni)->DeleteLocalRef(jni, buffer);
        t->room_wrapped = call->result->pointer;
    }
    jint n = (*jni)->CallStaticIntMethod(jni, t->bridge, t->call_into, t->functions[slot->index], t->arguments);
    status = ta_kotlin_threw(jni, &call->message);
    if (status != TA_OK) return status;
    if (n < 0) {
        call->result->length = (size_t)-(int64_t)n;
        return TA_ERROR_NO_MEMORY;
    }
    call->result->length = (size_t)n;
    return TA_OK;
}

/* The check pass the datamodel would make over a function's bytes: every
 * item read to the end, a tag other than 32 and a simple value other than
 * false, true, null and undefined refused. */
static ta_status ta_kotlin_check(ta_bytes result) {
    ta_cbor_reader r;
    ta_cbor_item it;
    int32_t got;
    ta_cbor_reader_init(&r, result);
    while ((got = ta_cbor_read(&r, &it)) == 1) {
        if (it.kind == TA_CBOR_TAG && it.value != TA_CBOR_TAG_URI) return TA_ERROR_INVALID;
        if (it.kind == TA_CBOR_SIMPLE && (it.value < TA_CBOR_FALSE || it.value > TA_CBOR_UNDEFINED))
            return TA_ERROR_INVALID;
    }
    return got == 0 ? TA_OK : (ta_status)got;
}

/* ---- the arguments, as the json datamodel would pass them ---------------------------------- */

static void ta_kotlin_items(ta_cbor_writer *w) {
    ta_cbor_write_array(w, 2);
    ta_cbor_write_map(w, 3);
    ta_cbor_write_text(w, ta_string("sku"));
    ta_cbor_write_text(w, ta_string("tea"));
    ta_cbor_write_text(w, ta_string("qty"));
    ta_cbor_write_uint(w, 2);
    ta_cbor_write_text(w, ta_string("cents"));
    ta_cbor_write_uint(w, 450);
    ta_cbor_write_map(w, 3);
    ta_cbor_write_text(w, ta_string("sku"));
    ta_cbor_write_text(w, ta_string("mug"));
    ta_cbor_write_text(w, ta_string("qty"));
    ta_cbor_write_uint(w, 1);
    ta_cbor_write_text(w, ta_string("cents"));
    ta_cbor_write_uint(w, 1200);
}

static void ta_kotlin_add(ta_cbor_writer *w) {
    ta_cbor_write_map(w, 2);
    ta_cbor_write_text(w, ta_string("sku"));
    ta_cbor_write_text(w, ta_string("tea"));
    ta_cbor_write_text(w, ta_string("qty"));
    ta_cbor_write_uint(w, 3);
}

/* function 0: [], 1: [items], 2: [items, {"sku":"tea","qty":3}],
 * 3: [{"sku":"tea","qty":3}] */
static ta_bytes ta_kotlin_arguments(uint32_t index, ta_cbor_writer *w, unsigned char *room) {
    ta_cbor_writer_init(w, (ta_buffer){room, 256, 0}, NULL);
    if (index == 3) {
        ta_cbor_write_array(w, 1);
        ta_kotlin_add(w);
        ta_bytes out = {0};
        ta_cbor_writer_bytes(w, &out);
        return out;
    }
    ta_cbor_write_array(w, index);
    if (index >= 1) ta_kotlin_items(w);
    if (index >= 2) {
        ta_cbor_write_map(w, 2);
        ta_cbor_write_text(w, ta_string("sku"));
        ta_cbor_write_text(w, ta_string("tea"));
        ta_cbor_write_text(w, ta_string("qty"));
        ta_cbor_write_uint(w, 3);
    }
    ta_bytes out = {0};
    ta_cbor_writer_bytes(w, &out);
    return out;
}

/* ---- the native methods -------------------------------------------------------------------- */

static void ta_kotlin_throw(JNIEnv *jni, const char *message) {
    jclass c = (*jni)->FindClass(jni, "java/lang/IllegalStateException");
    if (c) (*jni)->ThrowNew(jni, c, message);
}

JNIEXPORT void JNICALL Java_jsonbench_Native_register(JNIEnv *jni, jobject self, jobjectArray functions) {
    (void)self;
    ta_kotlin_table *t = &ta_kotlin_state;
    jsize n = (*jni)->GetArrayLength(jni, functions);
    if (n > TA_KOTLIN_FUNCTIONS) { ta_kotlin_throw(jni, "too many functions"); return; }
    jclass iface = (*jni)->FindClass(jni, "jsonbench/JsonFunction");
    t->call = (*jni)->GetMethodID(jni, iface, "call", "([B)[B");
    jclass bridge = (*jni)->FindClass(jni, "jsonbench/Bridge");
    t->bridge = (*jni)->NewGlobalRef(jni, bridge);
    t->call_buffer = (*jni)->GetStaticMethodID(jni, bridge, "callBuffer", "(Ljsonbench/JsonFunction;I)[B");
    if (!t->call || !t->call_buffer) return; /* NoSuchMethodError pending */
    t->staging = malloc(TA_KOTLIN_STAGING);
    jobject buffer = (*jni)->NewDirectByteBuffer(jni, t->staging, TA_KOTLIN_STAGING);
    jmethodID set = (*jni)->GetStaticMethodID(jni, bridge, "setStaging", "(Ljava/nio/ByteBuffer;)V");
    if (!set) return;
    (*jni)->CallStaticVoidMethod(jni, bridge, set, buffer);
    t->call_into = (*jni)->GetStaticMethodID(jni, bridge, "callInto", "(Ljsonbench/JsonFunction;[B)I");
    t->set_room = (*jni)->GetStaticMethodID(jni, bridge, "setRoom", "(Ljava/nio/ByteBuffer;)V");
    if (!t->call_into || !t->set_room) return;
    t->room = malloc(TA_KOTLIN_ROOM);
    if (!t->room) { ta_kotlin_throw(jni, "no memory for the room"); return; }
    jobject room = (*jni)->NewDirectByteBuffer(jni, t->room, TA_KOTLIN_ROOM);
    (*jni)->CallStaticVoidMethod(jni, bridge, t->set_room, room);
    t->room_buffer = (*jni)->NewGlobalRef(jni, room);
    (*jni)->DeleteLocalRef(jni, room);
    t->room_wrapped = t->room;
    for (jsize i = 0; i < n; i++) {
        jobject f = (*jni)->GetObjectArrayElement(jni, functions, i);
        t->functions[i] = (*jni)->NewGlobalRef(jni, f);
        (*jni)->DeleteLocalRef(jni, f);
        t->slots[i] = (ta_kotlin_slot){t, (uint32_t)i};
    }
    t->count = (uint32_t)n;
}

/* the trampoline through a pointer, as the datamodel calls it */
static ta_kotlin_writer_function volatile ta_kotlin_function = ta_kotlin_trampoline;

static ta_status ta_kotlin_once(ta_kotlin_slot *slot, ta_bytes arguments, ta_cbor_writer *result) {
    unsigned char room[512];
    ta_cbor_writer_init(result, (ta_buffer){room, sizeof room, 0}, NULL);
    ta_kotlin_call call = {sizeof call, 0, slot, arguments, result, {0}};
    ta_status s = ta_kotlin_function(&call);
    ta_bytes out;
    if (s == TA_OK) s = ta_cbor_writer_bytes(result, &out);
    return s;
}

static ta_kotlin_bytes_function volatile ta_kotlin_function_bytes = ta_kotlin_trampoline_bytes;
static ta_kotlin_bytes_function volatile ta_kotlin_function_bytes_direct = ta_kotlin_trampoline_bytes_direct;

/* one bytes-mode call into the driver's room; *out is the result's bytes */
static ta_status ta_kotlin_once_bytes(ta_kotlin_slot *slot, ta_bytes arguments, ta_bytes *out) {
    ta_kotlin_table *t = slot->table;
    ta_buffer result = {t->room, TA_KOTLIN_ROOM, 0};
    ta_kotlin_bytes_call call = {sizeof call, 0, slot, arguments, &result, {0}};
    ta_status s = t->mode == TA_KOTLIN_MODE_BYTES_DIRECT ? ta_kotlin_function_bytes_direct(&call)
                                                         : ta_kotlin_function_bytes(&call);
    if (s != TA_OK) return s;
    *out = (ta_bytes){result.pointer, result.length};
    if (t->mode == TA_KOTLIN_MODE_BYTES_CHECK) s = ta_kotlin_check(*out);
    return s;
}

static int ta_kotlin_bytes_mode(int mode) {
    return mode == TA_KOTLIN_MODE_BYTES || mode == TA_KOTLIN_MODE_BYTES_DIRECT ||
           mode == TA_KOTLIN_MODE_BYTES_CHECK;
}

/* one call in any mode; its result's bytes copied into out (out->length set) */
static ta_status ta_kotlin_one(ta_kotlin_slot *slot, ta_bytes arguments, ta_buffer *out) {
    ta_bytes bytes;
    ta_status s;
    unsigned char room[512];
    ta_cbor_writer result;
    if (ta_kotlin_bytes_mode(slot->table->mode)) {
        s = ta_kotlin_once_bytes(slot, arguments, &bytes);
    } else {
        ta_cbor_writer_init(&result, (ta_buffer){room, sizeof room, 0}, NULL);
        ta_kotlin_call call = {sizeof call, 0, slot, arguments, &result, {0}};
        s = ta_kotlin_function(&call);
        if (s == TA_OK) s = ta_cbor_writer_bytes(&result, &bytes);
    }
    if (s != TA_OK) return s;
    out->length = bytes.length;
    if (bytes.length > out->capacity) return TA_ERROR_NO_MEMORY;
    memcpy(out->pointer, bytes.pointer, bytes.length);
    return TA_OK;
}

JNIEXPORT jlong JNICALL Java_jsonbench_Native_bench(JNIEnv *jni, jobject self, jint index, jint n, jint mode) {
    (void)self;
    ta_kotlin_table *t = &ta_kotlin_state;
    if (index < 0 || (uint32_t)index >= t->count) { ta_kotlin_throw(jni, "no such function"); return -1; }
    t->jni = jni;
    t->mode = mode;
    unsigned char room[256];
    ta_cbor_writer args;
    ta_bytes arguments = ta_kotlin_arguments((uint32_t)index, &args, room);
    ta_kotlin_slot *slot = &t->slots[index];
    struct timespec a, b;
    if (ta_kotlin_bytes_mode(mode)) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (jint i = 0; i < n; i++) {
            ta_bytes out;
            ta_status s = ta_kotlin_once_bytes(slot, arguments, &out);
            if (s != TA_OK) { ta_kotlin_throw(jni, "a call failed"); return -1; }
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        return (jlong)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
    }
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (jint i = 0; i < n; i++) {
        ta_cbor_writer result;
        ta_status s = ta_kotlin_once(slot, arguments, &result);
        if (s != TA_OK) { ta_kotlin_throw(jni, "a call failed"); return -1; }
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (jlong)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
}

JNIEXPORT jstring JNICALL Java_jsonbench_Native_sample(JNIEnv *jni, jobject self, jint index, jint mode) {
    (void)self;
    ta_kotlin_table *t = &ta_kotlin_state;
    if (index < 0 || (uint32_t)index >= t->count) { ta_kotlin_throw(jni, "no such function"); return NULL; }
    t->jni = jni;
    t->mode = mode;
    unsigned char room[256], out_room[512], text[1024];
    ta_cbor_writer args;
    ta_bytes arguments = ta_kotlin_arguments((uint32_t)index, &args, room);
    ta_buffer result = {out_room, sizeof out_room, 0};
    ta_status s = ta_kotlin_one(&t->slots[index], arguments, &result);
    if (s != TA_OK) { ta_kotlin_throw(jni, "the call failed"); return NULL; }
    ta_bytes out = {out_room, result.length};
    ta_buffer diagnostic = {text, sizeof text - 1, 0};
    char line[2200];
    size_t k = 0;
    if (ta_cbor_diagnostic(arguments, &diagnostic) == TA_OK) {
        memcpy(line, text, diagnostic.length); k = diagnostic.length;
    }
    memcpy(line + k, " -> ", 4); k += 4;
    diagnostic = (ta_buffer){text, sizeof text - 1, 0};
    if (ta_cbor_diagnostic(out, &diagnostic) == TA_OK) { memcpy(line + k, text, diagnostic.length); k += diagnostic.length; }
    line[k] = 0;
    return (*jni)->NewStringUTF(jni, line);
}

/* one call's result bytes, to compare the modes */
JNIEXPORT jbyteArray JNICALL Java_jsonbench_Native_result(JNIEnv *jni, jobject self, jint index, jint mode) {
    (void)self;
    ta_kotlin_table *t = &ta_kotlin_state;
    if (index < 0 || (uint32_t)index >= t->count) { ta_kotlin_throw(jni, "no such function"); return NULL; }
    t->jni = jni;
    t->mode = mode;
    unsigned char room[256], out_room[1024];
    ta_cbor_writer args;
    ta_bytes arguments = ta_kotlin_arguments((uint32_t)index, &args, room);
    ta_buffer result = {out_room, sizeof out_room, 0};
    if (ta_kotlin_one(&t->slots[index], arguments, &result) != TA_OK) {
        ta_kotlin_throw(jni, "the call failed");
        return NULL;
    }
    jbyteArray bytes = (*jni)->NewByteArray(jni, (jsize)result.length);
    if (bytes) (*jni)->SetByteArrayRegion(jni, bytes, 0, (jsize)result.length, (const jbyte *)out_room);
    return bytes;
}
