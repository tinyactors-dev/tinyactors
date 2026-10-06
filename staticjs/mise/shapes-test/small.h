/* The event and operator names of a small staticjs.h, for shapes-test. */
typedef enum {
    SJS_EV_NUMBER = 1, /* number */
    SJS_EV_NAME,
    SJS_EV_MEMBER,
    SJS_EV_BINARY,
    SJS_EV_ARROW_BEGIN,
    SJS_EV_PARAM,
    SJS_EV_PARAMS_END,
    SJS_EV_ARROW_END,
    SJS_EV_BIND,
    SJS_EV_INDEX,
    SJS_EV_CALL_BEGIN,
    SJS_EV_ARGUMENT,
    SJS_EV_CALL_END,
    SJS_EV_END
} sjs_event_kind;
typedef enum { SJS_OP_ADD = 1, SJS_OP_SUBTRACT } sjs_operator;
