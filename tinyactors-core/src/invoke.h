#pragma once

#include "fault.h"

typedef struct {
    ta_invoke_id id;
    ta_atom type;
    uint64_t service_handle;
    bool autoforward;
    bool active;
} ta_active_invoke;

typedef struct {
    ta_atom type;
    ta_invoker invoker;
} ta_invoker_registration;
