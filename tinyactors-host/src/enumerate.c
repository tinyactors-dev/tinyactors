#include "tinyactors_host/enumerate.h"

#include <stddef.h>
#include "system_private.h"

typedef struct {
    tah_id_page *page;
} tah_id_visit;

/* Copies the visited actor's ID; the inspection itself is borrowed. */
static ta_result tah_visit_id(void *self, const ta_actor_inspection *inspection) {
    tah_id_visit *visit = self;
    tah_id_page *page = visit->page;
    uint32_t at = page->count * 2u;
    page->pairs[at] = ta_actor_id_slot(inspection->actor);
    page->pairs[at + 1u] = ta_actor_id_generation(inspection->actor);
    page->count += 1u;
    return TA_OK;
}

ta_result tah_enumerate_ids(tah_system *system, tah_id_page *page) {
    if (system == NULL || page == NULL || page->limit == 0 || page->pairs == NULL) {
        return TA_ERR_INVALID;
    }
    page->count = 0;
    page->next = page->from;
    if (system->inside_core) {
        return TA_ERR_BUSY;
    }
    tah_id_visit visit = { .page = page };
    system->inside_core = true;
    ta_result result = ta_system_visit_actors(
        system->core, &(ta_actor_visit_params){
                          .inspector = { .self = &visit, .visit = tah_visit_id },
                          .from = page->from,
                          .limit = page->limit,
                          .next = &page->next,
                      });
    tah_system_leave_core(system);
    return result;
}
