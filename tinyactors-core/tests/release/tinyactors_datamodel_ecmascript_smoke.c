/* The release's smoke test of libtinyactors_datamodel_ecmascript (mise run
 * release): built against the packed static and shared libraries, its own
 * and libtinyactors's, here and for each Linux release target in a
 * container. It uses the public headers alone: an expression evaluated by
 * QuickJS into a payload, written back as JSON. A program links -lm beside
 * the libraries (QuickJS's math is the C library's). */
#include "tinyactors_datamodel_ecmascript.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    ta_datamodel_ecmascript_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    ta_datamodel_ecmascript *ecmascript = ta_datamodel_ecmascript_new(&config);
    if (ecmascript == NULL) {
        fprintf(stderr, "tinyactors_datamodel_ecmascript smoke: no data model\n");
        return 1;
    }
    static const char EXPRESSION[] = "({sum: [1, 2, 3].reduce((a, b) => a + b), text: 'é'.normalize('NFD').length})";
    ta_string_view expression = {EXPRESSION, sizeof EXPRESSION - 1};
    ta_payload payload = ta_datamodel_ecmascript_payload_eval(ecmascript, expression);
    char json[128];
    size_t length = payload ? ta_datamodel_ecmascript_payload_to_json(ecmascript, payload, json, sizeof json) : 0;
    if (payload == 0 || length == (size_t)-1 || strcmp(json, "{\"sum\":6,\"text\":2}") != 0) {
        fprintf(stderr, "tinyactors_datamodel_ecmascript smoke: got %s\n", payload ? json : "no payload");
        return 1;
    }
    ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(ecmascript);
    binding.vtable->release(binding.datamodel, payload);
    ta_datamodel_ecmascript_free(ecmascript);
    printf("tinyactors_datamodel_ecmascript %d.%d.%d: QuickJS evaluated %s\n", TA_VERSION_MAJOR, TA_VERSION_MINOR,
           TA_VERSION_PATCH, json);
    return 0;
}
