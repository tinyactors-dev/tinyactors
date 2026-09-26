#pragma once

#include "actor.h"

/* Session images are opaque; struct ta_session_image is private to session_image.c. */

/*
 * The system's identity for images and offloaded bytes (ADR 0074, ADR
 * 0085): a token drawn once from its ID generator (ADR 0073), by its first
 * capture, and kept. An image or bytes name the system by its address and
 * this token.
 */
uint64_t ta_system_image_token(ta_system *system);

/* Whether `image` was captured in another system than `system` (ADR 0085). */
bool ta_image_foreign(const ta_session_image *image, const ta_system *system);
