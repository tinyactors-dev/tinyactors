/* The JNI half of the Kotlin bridge (a spike: tools/precompile): the
 * datamodel's one function calls tinyactors.Bridge.call, and the scripted
 * order runs with it (../demo). The JNIEnv is the thread's that called
 * runOrderDemo: core calls the datamodel on that thread, during the call. */
#include <jni.h>
#include <string.h>

#include <tinyactors/datamodel_precompiled.h>
#include <tinyactors/runtime.h>

#include "../../demo/demo.c"

typedef struct kotlin_bridge {
    JNIEnv *jni;
    jclass bridge;
    jmethodID call, room;
    void *room_pointer;
} kotlin_bridge;

/* the exception's text, into the request's message room */
static void kotlin_message(kotlin_bridge *k, ta_precompiled_request *request) {
    jthrowable thrown = (*k->jni)->ExceptionOccurred(k->jni);
    (*k->jni)->ExceptionClear(k->jni);
    jclass throwable = (*k->jni)->FindClass(k->jni, "java/lang/Throwable");
    jmethodID to_string = (*k->jni)->GetMethodID(k->jni, throwable, "toString", "()Ljava/lang/String;");
    jstring text = (jstring)(*k->jni)->CallObjectMethod(k->jni, thrown, to_string);
    const char *utf = text ? (*k->jni)->GetStringUTFChars(k->jni, text, NULL) : "an exception";
    size_t length = strlen(utf) < request->message.capacity ? strlen(utf) : request->message.capacity;
    memcpy(request->message.pointer, utf, length);
    request->message.length = length;
    if (text) (*k->jni)->ReleaseStringUTFChars(k->jni, text, utf);
}

static ta_status kotlin_function(void *context, ta_precompiled_request *request) {
    kotlin_bridge *k = (kotlin_bridge *)context;
    JNIEnv *jni = k->jni;
    if (k->room_pointer != request->output.pointer) { /* the room the layer lends, as a direct buffer, once */
        jobject buffer = (*jni)->NewDirectByteBuffer(jni, request->output.pointer, (jlong)request->output.capacity);
        (*jni)->CallStaticVoidMethod(jni, k->bridge, k->room, buffer);
        k->room_pointer = request->output.pointer;
    }
    jbyteArray input = NULL;
    if (request->input.length) {
        input = (*jni)->NewByteArray(jni, (jsize)request->input.length);
        (*jni)->SetByteArrayRegion(jni, input, 0, (jsize)request->input.length, (const jbyte *)request->input.pointer);
    }
    jlong answer = (*jni)->CallStaticLongMethod(jni, k->bridge, k->call, (jint)request->operation, (jint)request->chart,
                                                (jint)request->expression, (jlong)request->session, (jint)request->number,
                                                input, (jlong)(intptr_t)request);
    if (input) (*jni)->DeleteLocalRef(jni, input);
    if ((*jni)->ExceptionCheck(jni)) {
        kotlin_message(k, request);
        return TA_ERROR_EXECUTION;
    }
    switch (request->operation) {
    case TA_PRECOMPILED_ATTACH:
    case TA_PRECOMPILED_CONDITION: request->number = (uint32_t)answer; break;
    case TA_PRECOMPILED_CREATE:
    case TA_PRECOMPILED_DESERIALIZE: request->session = (uint64_t)answer; break;
    case TA_PRECOMPILED_VALUE:
    case TA_PRECOMPILED_STRING:
    case TA_PRECOMPILED_SERIALIZE:
    case TA_PRECOMPILED_VARIABLE:
        if (answer < 0 || (size_t)answer > request->output.capacity) return TA_ERROR_NO_MEMORY;
        request->output.length = (size_t)answer;
        break;
    default: break;
    }
    return TA_OK;
}

JNIEXPORT jint JNICALL Java_tinyactors_Native_runOrderDemo(JNIEnv *jni, jclass native, jstring path) {
    (void)native;
    kotlin_bridge k = {jni, (*jni)->FindClass(jni, "tinyactors/Bridge"), 0, 0, 0};
    k.call = (*jni)->GetStaticMethodID(jni, k.bridge, "call", "(IIIJI[BJ)J");
    k.room = (*jni)->GetStaticMethodID(jni, k.bridge, "room", "(Ljava/nio/ByteBuffer;)V");
    ta_datamodel_precompiled_config config = {sizeof config, 0, "kotlin", kotlin_function, &k, ta_runtime_malloc()};
    ta_datamodel_precompiled *datamodel;
    if (ta_datamodel_precompiled_new(&config, &datamodel) != TA_OK) return 1;
    const char *chart = (*jni)->GetStringUTFChars(jni, path, NULL);
    int result = ta_demo_order(chart, ta_datamodel_precompiled_binding(datamodel));
    (*jni)->ReleaseStringUTFChars(jni, path, chart);
    ta_datamodel_precompiled_free(datamodel);
    return result;
}

JNIEXPORT jboolean JNICALL Java_tinyactors_Native_inState(JNIEnv *jni, jclass native, jlong request, jstring state) {
    (void)native;
    const char *name = (*jni)->GetStringUTFChars(jni, state, NULL);
    int in = ta_datamodel_precompiled_in((const ta_precompiled_request *)(intptr_t)request, ta_string(name));
    (*jni)->ReleaseStringUTFChars(jni, state, name);
    return in ? JNI_TRUE : JNI_FALSE;
}
