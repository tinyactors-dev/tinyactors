/* payload_text.h — payloads of the ECMAScript datamodel as text, for tests:
 * the neutral value (to_wire, CBOR) in diagnostic notation
 * (ta_cbor_diagnostic), which reads as compact JSON for JSON-shaped values. */
#ifndef TINYACTORS_TESTS_SUPPORT_PAYLOAD_TEXT_H
#define TINYACTORS_TESTS_SUPPORT_PAYLOAD_TEXT_H

#include <stdlib.h>

#include <tinyactors/cbor.h>
#include <tinyactors/datamodel_ecmascript.h>

#ifndef TEST_UNUSED
#define TEST_UNUSED __attribute__((unused))
#endif

/* The payload's text into buffer, NUL-terminated and cut to fit; returns its
 * whole length, (size_t)-1 when it is not a neutral value. */
TEST_UNUSED static size_t payload_text(ta_datamodel_ecmascript *ecmascript, ta_payload payload,
                                       char *buffer, size_t capacity) {
    ta_bytes cbor = {NULL, 0}; /* the datamodel's view */
    if (capacity) buffer[0] = 0;
    ta_status status = ta_datamodel_ecmascript_payload_to_cbor(ecmascript, payload, &cbor);
    ta_buffer out = {buffer, capacity ? capacity - 1 : 0, 0};
    if (status == TA_OK) status = ta_cbor_diagnostic(cbor, &out);
    if (status != TA_OK && status != TA_ERROR_NO_MEMORY) return (size_t)-1;
    if (capacity) buffer[out.length < capacity - 1 ? out.length : capacity - 1] = 0;
    return out.length;
}

/* An expression evaluated in a session (instance 0: outside any), as
 * payload_text writes it; (size_t)-1 when it fails. */
TEST_UNUSED static size_t eval_text(ta_datamodel_ecmascript *ecmascript, uint64_t instance,
                                    ta_string_view expression, char *buffer, size_t capacity) {
    if (capacity) buffer[0] = 0;
    ta_payload payload = ta_datamodel_ecmascript_payload_eval(ecmascript, instance, expression);
    if (!payload) return (size_t)-1;
    size_t length = payload_text(ecmascript, payload, buffer, capacity);
    ta_datamodel_ecmascript_vtable()->release(ecmascript, payload);
    return length;
}

#endif /* TINYACTORS_TESTS_SUPPORT_PAYLOAD_TEXT_H */
