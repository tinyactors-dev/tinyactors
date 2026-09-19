#pragma once

/*
 * The RPC codec's only serialization boundary for ta_value. Each data model
 * converts its own values to and from wire JSON; neither model knows the
 * other's values. Encode picks the conversion by the value's vtable; decode
 * uses the data model of the actor (or definition) that receives the value.
 * The wire format carries no model tag.
 */

#include <stdbool.h>

#include "jv.h"
#include "tinyactors.h"

typedef struct tat_bind tat_bind;

typedef struct {
    const char *name;
    /* True for values of this model. */
    bool (*owns)(const ta_value *value);
    /* Returns false when `value` is not this model's value; *out is owned. */
    bool (*encode)(const ta_value *value, jv *out);
    /* `model` is the ta_datamodel.self of this model; `json` is borrowed. */
    ta_result (*decode)(void *model, jv json, ta_value *out);
    /* The adapter vtable that selects this codec for decoding. */
    const ta_datamodel_vtable *(*datamodel)(void);
} tat_value_codec;

/* Wire JSON of a value: its model's conversion, or {"$value": "foreign"}. */
jv tat_value_encode(const ta_value *value);
/* Decodes with `model`'s conversion; TA_ERR_UNSUPPORTED for an unknown model. */
ta_result tat_value_decode(ta_datamodel model, jv json, ta_value *out);
/* The codec that owns `value`, or null. */
const tat_value_codec *tat_value_codec_for_value(const ta_value *value);
/* The codec for values of `model`, or null. */
const tat_value_codec *tat_value_codec_for_model(ta_datamodel model);

/* The data model an actor's definition was compiled with. */
ta_result tat_actor_datamodel(ta_system *system, ta_actor_id actor, ta_datamodel *out);

/*
 * Before a request is dispatched: selects bind->decode_model from the
 * request's receiving actor or definition, or the jq model when the method
 * names none.
 */
void tat_value_codec_select(tat_bind *bind, const char *method, jv params);
