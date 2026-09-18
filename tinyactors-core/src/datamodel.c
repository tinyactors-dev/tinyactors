#include "datamodel.h"
#include "actor.h"

bool ta_dm_host_in_state(const ta_dm_host *host, ta_state_id state) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return false;
    }
    return ta_actor_state_active(host->impl->actor, state);
}

const ta_event *ta_dm_host_event(const ta_dm_host *host) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL ||
        !host->impl->actor->has_current_event) {
        return NULL;
    }
    return &host->impl->actor->current_event;
}

ta_actor_id ta_dm_host_actor(const ta_dm_host *host) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return 0;
    }
    return host->impl->actor->record.id;
}
