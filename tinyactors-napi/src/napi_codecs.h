#pragma once
/*
 * A chart codec written in JavaScript (napi ADR 0027, core ADR 0053).
 *
 * System.registerCodec() keeps the codec in lib/; for one define() or one
 * Definition.encode(), napi_system.c wraps its functions in a core
 * `ta_chart_codec` whose decode and encode call them synchronously, inside
 * ta_definition_import() and ta_definition_encode(), so a JavaScript codec
 * takes the same path as the SCXML codec. lib/ hands over functions that
 * never throw a codec's refusal: they answer plain objects (see lib/system.js,
 * #codecCall), and any other exception stays pending for the method to
 * rethrow.
 *
 * The spec crosses through the generated codecs (tna_gen_decode_spec,
 * tna_gen_encode_spec) under `call`, whose arena holds a decoded spec until
 * the call ends, after core has compiled it.
 */
#include <node_api.h>

#include "runtime.h"
#include "tinyactors.h"

/* The longest codec name napi copies into the adapter's info. */
#define TNA_CODEC_NAME_MAX 256u

typedef struct {
    tna_call *call;
    /* decode(bytes) → { spec } or { refused }; NULL when the codec only encodes. */
    napi_value decode;
    /* encode(spec) → { bytes, spans? } or { refused }; NULL when it only decodes. */
    napi_value encode;
    ta_chart_codec_vtable vtable;
    char name[TNA_CODEC_NAME_MAX];
} tna_js_codec;

/*
 * Makes `codec` an adapter over `decode` and `encode` (either may be NULL),
 * named `name`, for the length of `call`. TA_ERR_INVALID for a name longer
 * than TNA_CODEC_NAME_MAX - 1 bytes.
 */
ta_result tna_js_codec_init(tna_js_codec *codec, tna_call *call, ta_str name, napi_value decode, napi_value encode,
                            ta_chart_codec *out);

/* A chart error code's name in index.d.ts (ChartErrorCode), or NULL for NONE. */
const char *tna_chart_error_name(ta_chart_error_code code);
