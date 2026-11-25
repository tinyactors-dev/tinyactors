/* cc main.c host.c -ltinyactors_datamodel_ecmascript -ltinyactors_runtime -ltinyactors -lm */
#include <stdio.h>
#include "host.h"

typedef struct payments {
    int charges;
} payments;

/* What the order sends out, and how this program answers. */
static void handle(host *h, uint32_t effect) {
    payments *service = h->application;
    switch (ta_host_effect_number(h->host, effect, TA_EFFECT_FIELD_KIND)) {
    case TA_EFFECT_SEND: /* the chart's <send type="payments"> */
        host_print(h, "charge", effect);
        if (++service->charges == 1) host_fail(h, effect); /* down: the order gets error.communication */
        else host_answer(h, effect, "charged"); /* an event back to the order */
        break;
    case TA_EFFECT_LOG: { /* the chart's <log> */
        ta_string_view text = ta_host_effect_text(h->host, effect, TA_EFFECT_FIELD_EVENT);
        printf("log %.*s\n", (int)text.length, text.pointer);
        break;
    }
    case TA_EFFECT_DONE: /* the order reached <final id="paid">: its total */
        host_print(h, "paid", effect);
        break;
    default:
        break;
    }
}

int main(void) {
    payments service = {0};
    host h = {.application = &service};
    if (host_open(&h, &(host_options){"order.scxml", "shop", "payments"}) != TA_OK) return 1;
    uint64_t order = host_spawn(&h, "order");
    host_send(&h, (host_event){order, "add", "({item: 'notebook', price: 30})"});
    host_send(&h, (host_event){order, "add", "({item: 'pen', price: 12})"});
    host_send(&h, (host_event){order, "checkout", NULL});
    host_run(&h, handle); /* until the order is paid: it retries on its own */
    host_close(&h);
    return 0;
}
