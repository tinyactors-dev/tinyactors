/* The native side of the Kotlin spike: a JNI library whose bench() loops
 * over a C trampoline with ta_json_function's signature. The trampoline's
 * context is a slot naming an index into the table of Kotlin functions;
 * it calls the function through JNI and re-emits the CBOR item it returns
 * into call->result, walking it with ta_cbor_reader and writing each item
 * with ta_cbor_writer (the writer has no raw-bytes call), which also
 * checks what Kotlin wrote. */
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <tinyactors/cbor.h>
#include <tinyactors/datamodel_json.h>

#define TA_KOTLIN_FUNCTIONS 8
#define TA_KOTLIN_STAGING 4096

typedef struct ta_kotlin_table ta_kotlin_table;

typedef struct ta_kotlin_slot {
    ta_kotlin_table *table;
    uint32_t index;
} ta_kotlin_slot;

struct ta_kotlin_table {
    JNIEnv *env;     /* the calling thread's, set by bench() */
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

static ta_status ta_kotlin_trampoline(ta_json_call *call) {
    ta_kotlin_slot *slot = call->context;
    ta_kotlin_table *t = slot->table;
    JNIEnv *env = t->env;
    jsize length = (jsize)call->arguments.length;
    jobject function = t->functions[slot->index];
    jbyteArray result;
    if (t->mode == 0) {
        if (length != t->arguments_length) { /* reused while the length holds */
            jbyteArray fresh = (*env)->NewByteArray(env, length);
            if (!fresh) return TA_ERROR_NO_MEMORY;
            if (t->arguments) (*env)->DeleteGlobalRef(env, t->arguments);
            t->arguments = (*env)->NewGlobalRef(env, fresh);
            (*env)->DeleteLocalRef(env, fresh);
            t->arguments_length = length;
        }
        (*env)->SetByteArrayRegion(env, t->arguments, 0, length, call->arguments.pointer);
        result = (*env)->CallObjectMethod(env, function, t->call, t->arguments);
    } else {
        if (length > TA_KOTLIN_STAGING) return TA_ERROR_NO_MEMORY;
        memcpy(t->staging, call->arguments.pointer, (size_t)length);
        result = (*env)->CallStaticObjectMethod(env, t->bridge, t->call_buffer, function, length);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env); /* a binding would keep its message */
        call->message = ta_string("the Kotlin function threw");
        return TA_ERROR_INVALID;
    }
    jsize n = (*env)->GetArrayLength(env, result);
    unsigned char room[1024];
    ta_status status;
    if ((size_t)n <= sizeof room) {
        (*env)->GetByteArrayRegion(env, result, 0, n, (jbyte *)room);
        status = ta_kotlin_copy_item(call->result, (ta_bytes){room, (size_t)n});
    } else {
        jbyte *p = (*env)->GetPrimitiveArrayCritical(env, result, NULL);
        status = ta_kotlin_copy_item(call->result, (ta_bytes){p, (size_t)n});
        (*env)->ReleasePrimitiveArrayCritical(env, result, p, JNI_ABORT);
    }
    (*env)->DeleteLocalRef(env, result);
    return status;
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

/* function 0: [], 1: [items], 2: [items, {"sku":"tea","qty":3}] */
static ta_bytes ta_kotlin_arguments(uint32_t index, ta_cbor_writer *w, unsigned char *room) {
    ta_cbor_writer_init(w, (ta_buffer){room, 256, 0}, NULL);
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

static void ta_kotlin_throw(JNIEnv *env, const char *message) {
    jclass c = (*env)->FindClass(env, "java/lang/IllegalStateException");
    if (c) (*env)->ThrowNew(env, c, message);
}

JNIEXPORT void JNICALL Java_jsonbench_Native_register(JNIEnv *env, jobject self, jobjectArray functions) {
    (void)self;
    ta_kotlin_table *t = &ta_kotlin_state;
    jsize n = (*env)->GetArrayLength(env, functions);
    if (n > TA_KOTLIN_FUNCTIONS) { ta_kotlin_throw(env, "too many functions"); return; }
    jclass iface = (*env)->FindClass(env, "jsonbench/JsonFunction");
    t->call = (*env)->GetMethodID(env, iface, "call", "([B)[B");
    jclass bridge = (*env)->FindClass(env, "jsonbench/Bridge");
    t->bridge = (*env)->NewGlobalRef(env, bridge);
    t->call_buffer = (*env)->GetStaticMethodID(env, bridge, "callBuffer", "(Ljsonbench/JsonFunction;I)[B");
    if (!t->call || !t->call_buffer) return; /* NoSuchMethodError pending */
    t->staging = malloc(TA_KOTLIN_STAGING);
    jobject buffer = (*env)->NewDirectByteBuffer(env, t->staging, TA_KOTLIN_STAGING);
    jmethodID set = (*env)->GetStaticMethodID(env, bridge, "setStaging", "(Ljava/nio/ByteBuffer;)V");
    if (!set) return;
    (*env)->CallStaticVoidMethod(env, bridge, set, buffer);
    for (jsize i = 0; i < n; i++) {
        jobject f = (*env)->GetObjectArrayElement(env, functions, i);
        t->functions[i] = (*env)->NewGlobalRef(env, f);
        (*env)->DeleteLocalRef(env, f);
        t->slots[i] = (ta_kotlin_slot){t, (uint32_t)i};
    }
    t->count = (uint32_t)n;
}

/* the trampoline through a pointer, as the datamodel calls it */
static ta_json_function volatile ta_kotlin_function = ta_kotlin_trampoline;

static ta_status ta_kotlin_once(ta_kotlin_slot *slot, ta_bytes arguments, ta_cbor_writer *result) {
    unsigned char room[512];
    ta_cbor_writer_init(result, (ta_buffer){room, sizeof room, 0}, NULL);
    ta_json_call call = {sizeof call, 0, slot, arguments, result, {0}};
    ta_status s = ta_kotlin_function(&call);
    ta_bytes out;
    if (s == TA_OK) s = ta_cbor_writer_bytes(result, &out);
    return s;
}

JNIEXPORT jlong JNICALL Java_jsonbench_Native_bench(JNIEnv *env, jobject self, jint index, jint n, jint mode) {
    (void)self;
    ta_kotlin_table *t = &ta_kotlin_state;
    if (index < 0 || (uint32_t)index >= t->count) { ta_kotlin_throw(env, "no such function"); return -1; }
    t->env = env;
    t->mode = mode;
    unsigned char room[256];
    ta_cbor_writer args;
    ta_bytes arguments = ta_kotlin_arguments((uint32_t)index, &args, room);
    ta_kotlin_slot *slot = &t->slots[index];
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (jint i = 0; i < n; i++) {
        ta_cbor_writer result;
        ta_status s = ta_kotlin_once(slot, arguments, &result);
        if (s != TA_OK) { ta_kotlin_throw(env, "a call failed"); return -1; }
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (jlong)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
}

JNIEXPORT jstring JNICALL Java_jsonbench_Native_sample(JNIEnv *env, jobject self, jint index, jint mode) {
    (void)self;
    ta_kotlin_table *t = &ta_kotlin_state;
    if (index < 0 || (uint32_t)index >= t->count) { ta_kotlin_throw(env, "no such function"); return NULL; }
    t->env = env;
    t->mode = mode;
    unsigned char room[256], out_room[512], text[1024];
    ta_cbor_writer args, result;
    ta_bytes arguments = ta_kotlin_arguments((uint32_t)index, &args, room);
    ta_cbor_writer_init(&result, (ta_buffer){out_room, sizeof out_room, 0}, NULL);
    ta_json_call call = {sizeof call, 0, &t->slots[index], arguments, &result, {0}};
    ta_status s = ta_kotlin_function(&call);
    ta_bytes out;
    if (s == TA_OK) s = ta_cbor_writer_bytes(&result, &out);
    if (s != TA_OK) { ta_kotlin_throw(env, "the call failed"); return NULL; }
    ta_buffer diag = {text, sizeof text - 1, 0};
    ta_buffer in = {text, 0, 0};
    (void)in;
    char line[2200];
    size_t k = 0;
    if (ta_cbor_diagnostic(arguments, &diag) == TA_OK) {
        memcpy(line, text, diag.length); k = diag.length;
    }
    memcpy(line + k, " -> ", 4); k += 4;
    diag = (ta_buffer){text, sizeof text - 1, 0};
    if (ta_cbor_diagnostic(out, &diag) == TA_OK) { memcpy(line + k, text, diag.length); k += diag.length; }
    line[k] = 0;
    return (*env)->NewStringUTF(env, line);
}
