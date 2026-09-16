#pragma once

#include "types.h"


#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tinyactors core deliberately defines no persistence-store protocol.  This
 * compatibility module remains empty while durable checkpointing, command
 * sourcing, and storage adapters live in optional higher-level components.
 */

#ifdef __cplusplus
}
#endif
