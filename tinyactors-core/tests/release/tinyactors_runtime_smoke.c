/* The release's smoke test of libtinyactors_runtime (mise run release):
 * built for each Linux release target against the packed static and shared
 * libraries, its own and libtinyactors's, and run in a container of that
 * architecture. It uses the public headers alone: a chart compiled, an
 * actor spawned on a growable system, sent an event, and run to its final
 * state, which it reports as done. */
#include "tinyactors_runtime.h"

#include <stdio.h>
#include <string.h>

static const char CHART[] = "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='null' initial='idle'>"
                            "<state id='idle'><transition event='go' target='done'/></state>"
                            "<final id='done'/></scxml>";

int main(void) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.alloc = ta_runtime_malloc();
    ta_source source = {{"smoke.scxml", 11}, {CHART, sizeof CHART - 1}};
    ta_compile_result *compiled = NULL;
    if (ta_compile_charts(&options, (ta_source_list){&source, 1}, &compiled) != TA_OK) {
        fprintf(stderr, "tinyactors_runtime smoke: the chart did not compile\n");
        return 1;
    }
    ta_string_view bytes = ta_compile_image(compiled);
    ta_image image;
    if (ta_image_open(bytes.pointer, bytes.length, &image) != TA_OK) {
        fprintf(stderr, "tinyactors_runtime smoke: the image did not open\n");
        return 1;
    }
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.ticks_per_second = 1000;
    config.max_actors = 1;
    config.max_events = 4;
    ta_system *system = NULL;
    uint32_t actor = 0;
    ta_string_view name = {"smoke", 5};
    if (ta_runtime_system_new(&(ta_system_setup){{0, 0}, &image, &config, NULL, 0, 0}, &system) !=
            TA_OK ||
        ta_system_spawn(system, &(ta_spawn){0, 0, name, 0, (ta_string_view){0, 0}}, &actor) !=
            TA_OK) {
        fprintf(stderr, "tinyactors_runtime smoke: no actor\n");
        return 1;
    }
    ta_runtime_run(&system, 1000);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = actor;
    event.name.pointer = "go";
    event.name.length = 2;
    if (ta_system_send(system, &event) != TA_OK) {
        fprintf(stderr, "tinyactors_runtime smoke: the event was refused\n");
        return 1;
    }
ta_runtime_run(&system, 1000);
/* Its final state ends the session, which reports done. */
const ta_effect *effects;
uint32_t count = ta_system_effects(system, &effects);
int done = 0;
for (uint32_t i = 0; i < count; i++) done |= effects[i].kind == TA_EFFECT_DONE;
ta_system_effects_ack(system, count);
if (!done) {
    fprintf(stderr, "tinyactors_runtime smoke: the actor did not reach its final state\n");
    return 1;
}
    ta_runtime_system_free(system);
    ta_compile_free(compiled);
    printf("tinyactors_runtime %d.%d.%d: compiled a chart, ran an actor to its final state\n",
           TA_VERSION_MAJOR, TA_VERSION_MINOR, TA_VERSION_PATCH);
    return 0;
}
