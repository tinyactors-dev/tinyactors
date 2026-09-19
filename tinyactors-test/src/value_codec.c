#include "value_codec.h"

#include <string.h>

#include "binding.h"
#include "ecmascript_datamodel.h"
#include "json_datamodel_jv.h"

static bool tat_jq_owns(const ta_value *value) {
    jv probe;
    if (!tat_json_value_to_jv(value, &probe)) {
        return false;
    }
    jv_free(probe);
    return true;
}

static ta_result tat_jq_decode(void *model, jv json, ta_value *out) {
    return tat_json_value_from_jv(model, jv_copy(json), out);
}

static const ta_datamodel_vtable *tat_jq_datamodel(void) {
    return tat_json_datamodel(NULL).v;
}

static bool tat_js_encode(const ta_value *value, jv *out) {
    if (!tat_js_value_is(value)) {
        return false;
    }
    tat_js_json json = {0};
    if (tat_js_value_to_json(value, &json) != TA_OK) {
        *out = jv_object_set(jv_object(), jv_string("$value"), jv_string("unserializable"));
        return true;
    }
    jv parsed = json.len > INT32_MAX ? jv_invalid() : jv_parse_sized(json.ptr, (int)json.len);
    tat_js_json_release(&json);
    if (!jv_is_valid(parsed)) {
        jv_free(parsed);
        parsed = jv_object_set(jv_object(), jv_string("$value"), jv_string("unserializable"));
    }
    *out = parsed;
    return true;
}

static ta_result tat_js_decode(void *model, jv json, ta_value *out) {
    jv text = jv_dump_string(jv_copy(json), 0);
    const char *chars = jv_string_value(text);
    ta_result result = tat_js_value_from_json(model, (ta_str){.ptr = chars, .len = (uint32_t)strlen(chars)}, out);
    jv_free(text);
    return result;
}

static const ta_datamodel_vtable *tat_js_model(void) {
    return tat_js_datamodel(NULL).v;
}

static const tat_value_codec TAT_VALUE_CODECS[] = {
    {"jq", tat_jq_owns, tat_json_value_to_jv, tat_jq_decode, tat_jq_datamodel},
    {"ecmascript", tat_js_value_is, tat_js_encode, tat_js_decode, tat_js_model},
};

enum { TAT_VALUE_CODEC_COUNT = sizeof(TAT_VALUE_CODECS) / sizeof(TAT_VALUE_CODECS[0]) };

const tat_value_codec *tat_value_codec_for_value(const ta_value *value) {
    for (size_t index = 0; index < TAT_VALUE_CODEC_COUNT; index += 1) {
        if (TAT_VALUE_CODECS[index].owns(value)) {
            return &TAT_VALUE_CODECS[index];
        }
    }
    return NULL;
}

const tat_value_codec *tat_value_codec_for_model(ta_datamodel model) {
    for (size_t index = 0; index < TAT_VALUE_CODEC_COUNT; index += 1) {
        if (model.v != NULL && TAT_VALUE_CODECS[index].datamodel() == model.v) {
            return &TAT_VALUE_CODECS[index];
        }
    }
    return NULL;
}

jv tat_value_encode(const ta_value *value) {
    if (value == NULL || value->v == NULL) {
        return jv_null();
    }
    for (size_t index = 0; index < TAT_VALUE_CODEC_COUNT; index += 1) {
        jv out;
        if (TAT_VALUE_CODECS[index].encode(value, &out)) {
            return out;
        }
    }
    return jv_object_set(jv_object(), jv_string("$value"), jv_string("foreign"));
}

ta_result tat_value_decode(ta_datamodel model, jv json, ta_value *out) {
    const tat_value_codec *codec = tat_value_codec_for_model(model);
    if (codec == NULL || model.self == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    return codec->decode(model.self, json, out);
}

ta_result tat_actor_datamodel(ta_system *system, ta_actor_id actor, ta_datamodel *out) {
    ta_actor_inspection inspection;
    ta_result result = ta_actor_inspect(system, actor, &inspection);
    if (result != TA_OK) {
        return result;
    }
    ta_definition_inspection definition;
    result = ta_definition_inspect(system, inspection.definition_id, &definition);
    if (result == TA_OK) {
        *out = definition.datamodel;
    }
    return result;
}

/* A u64/u32 member encoded as a JSON number or a decimal string. */
static bool tat_member_id(jv params, const char *name, uint64_t *out) {
    jv value = jv_object_get(jv_copy(params), jv_string(name));
    bool ok = false;
    if (jv_get_kind(value) == JV_KIND_NUMBER) {
        double number = jv_number_value(value);
        ok = number >= 0 && number <= 9007199254740991.0 && number == (double)(uint64_t)number;
        *out = (uint64_t)number;
    } else if (jv_get_kind(value) == JV_KIND_STRING) {
        const char *text = jv_string_value(value);
        uint64_t parsed = 0;
        ok = *text != '\0';
        for (const char *c = text; *c != '\0' && ok; c += 1) {
            ok = *c >= '0' && *c <= '9' && parsed <= (UINT64_MAX - (uint64_t)(*c - '0')) / 10u;
            parsed = parsed * 10u + (uint64_t)(*c - '0');
        }
        *out = parsed;
    }
    jv_free(value);
    return ok;
}

void tat_value_codec_select(tat_bind *bind, const char *method, jv params) {
    bind->decode_model = tat_json_datamodel(&bind->json);
    /* The receivers come from the value models in the API description. */
    for (size_t index = 0; index < TAT_VALUE_RECEIVER_COUNT; index += 1) {
        const tat_value_receiver *receiver = &TAT_VALUE_RECEIVERS[index];
        if (strcmp(method, receiver->method) != 0) {
            continue;
        }
        jv system_handle = jv_object_get(jv_copy(params), jv_string("system"));
        void *system = NULL;
        bool resolved = jv_is_valid(system_handle) && tat_handle_resolve(bind, system_handle, "system", &system);
        jv_free(system_handle);
        /* A bad handle is the handler's error to report, not ours. */
        jv_free(bind->error);
        bind->error = jv_invalid();
        uint64_t id = 0;
        if (!resolved || !tat_member_id(params, receiver->member, &id)) {
            return;
        }
        ta_datamodel model = {0};
        ta_result result = TA_ERR_NOT_FOUND;
        if (receiver->model == TAT_VALUE_MODEL_ACTOR) {
            result = tat_actor_datamodel(system, (ta_actor_id)id, &model);
        } else {
            ta_definition_inspection definition;
            result = ta_definition_inspect(system, (ta_definition_id)id, &definition);
            model = definition.datamodel;
        }
        if (result == TA_OK && tat_value_codec_for_model(model) != NULL) {
            bind->decode_model = model;
        }
        return;
    }
}
