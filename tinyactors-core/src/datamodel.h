#pragma once

#include "configuration.h"
#include "event.h"
#include "fault.h"
#include "../include/tinyactors.h"

typedef struct ta_actor ta_actor;

/* The data-model host view resolves everything through the calling actor. */
typedef struct {
    ta_system *system;
    ta_actor *actor;
} ta_dm_host_impl;

struct ta_dm_host {
    const ta_dm_host_impl *impl;
};
