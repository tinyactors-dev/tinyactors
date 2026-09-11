/* Two host-defined, send-only processors. Registration supplies routing names,
 * not transport implementations: the host dispatches ta_io_next requests by
 * their canonical type URI to its WebSocket or pub/sub driver. */
#include "../src/tinyactors.h"

/* base must have been initialized with ta_system_config_init and configured
 * with the host's session prefix, data model and callbacks. This example
 * replaces its optional processor registrations; SCXML remains built in.
 * Calling create here lets all the nested arrays have automatic lifetime. */
ta_status_t create_transport_system(ta_arena_t *parent,
                                    const ta_system_config_t *base,
                                    ta_system_t **out_system)
{
    ta_system_config_t config = *base;
    config.ioprocessors = (const ta_ioprocessor_desc_t[]) {
        {
            .type = "urn:tinyactors:example:websocket",
            .aliases = (const char *const[]) { "websocket", "ws", NULL }
        },
        {
            .type = "urn:tinyactors:example:pubsub",
            .aliases = (const char *const[]) { "pubsub", NULL }
        },
        {0}
    };
    return ta_system_create(parent, &config, out_system);
}
