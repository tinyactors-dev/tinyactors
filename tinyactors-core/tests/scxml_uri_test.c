/*
 * The SCXML target URI grammar (ADR 0044). Core owns it, so this is the one
 * place it is specified: every spelling of a `<send>` target and every URI
 * core writes for a chart goes through these two functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyactors.h"

static uint32_t failures;

static void check(bool condition, const char *what) {
    if (!condition) {
        printf("scxml uri test: %s\n", what);
        failures += 1;
    }
}

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    return malloc(size);
}

static void test_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const ta_allocator_vtable ALLOCATOR_VTABLE = { .alloc = test_alloc, .free = test_free };

static ta_str str(const char *text) {
    return (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) };
}

static bool str_is(ta_str text, const char *expected) {
    return text.len == strlen(expected) && memcmp(text.ptr, expected, text.len) == 0;
}

/* The URI of a target, or "" when its kind has no URI. */
static const char *format(ta_system *system, ta_scxml_target target, char *buffer, uint32_t capacity) {
    uint32_t needed = 0;
    ta_result result = ta_scxml_target_format(
        system, &(ta_scxml_target_format_params){ .target = target, .buffer = buffer,
                                                  .capacity = capacity, .needed = &needed });
    if (result != TA_OK) {
        buffer[0] = '\0';
    }
    return buffer;
}

int main(void) {
    ta_system *system = NULL;
    ta_allocator allocator = { .self = NULL, .v = &ALLOCATOR_VTABLE };
    if (ta_system_create(&(ta_system_config){ .allocator = allocator, .initial_time = 0 }, &system) != TA_OK) {
        printf("scxml uri test: could not create a system\n");
        return 1;
    }

    ta_scxml_target target = { 0 };
    check(ta_scxml_target_parse(system, str("#_internal"), &target) == TA_OK &&
              target.kind == TA_SCXML_TARGET_INTERNAL,
          "#_internal");
    check(ta_scxml_target_parse(system, str("#_parent"), &target) == TA_OK &&
              target.kind == TA_SCXML_TARGET_PARENT,
          "#_parent");
    check(ta_scxml_target_parse(system, str("#_scxml_42"), &target) == TA_OK &&
              target.kind == TA_SCXML_TARGET_ACTOR && target.as.actor.kind == TA_ACTOR_REF_RUNTIME &&
              target.as.actor.as.runtime == 42,
          "#_scxml_42");
    check(ta_scxml_target_parse(system, str("#_worker"), &target) == TA_OK &&
              target.kind == TA_SCXML_TARGET_INVOKE,
          "#_<invokeid>");

    /* A named actor's logical address; family and name borrow from the text. */
    check(ta_scxml_target_parse(system, str("#_actor_door/front"), &target) == TA_OK &&
              target.kind == TA_SCXML_TARGET_ACTOR && target.as.actor.kind == TA_ACTOR_REF_NAMED &&
              str_is(target.as.actor.as.named.family.name, "door") &&
              str_is(target.as.actor.as.named.name, "front"),
          "#_actor_door/front");

    /*
     * A session URI whose text is not a session ID names no session (SCXML
     * C.1, IRP test496): it is inaccessible, not invalid, so it parses as
     * runtime actor 0, which never exists.
     */
    const char *inaccessible[] = { "#_scxml_", "#_scxml_4x", "#_scxml_foo" };
    for (size_t index = 0; index < sizeof(inaccessible) / sizeof(inaccessible[0]); index += 1) {
        check(ta_scxml_target_parse(system, str(inaccessible[index]), &target) == TA_OK &&
                  target.kind == TA_SCXML_TARGET_ACTOR &&
                  target.as.actor.kind == TA_ACTOR_REF_RUNTIME && target.as.actor.as.runtime == 0,
              inaccessible[index]);
    }

    /* Anything else is an invalid target (SCXML 6.2, IRP test194). */
    const char *invalid[] = { "elsewhere", "", "#_", "#_actor_", "#_actor_door", "#_actor_/front",
                              "#_actor_door/", "#_actor_a/b/c" };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index += 1) {
        check(ta_scxml_target_parse(system, str(invalid[index]), &target) == TA_ERR_INVALID,
              invalid[index][0] == '\0' ? "(empty)" : invalid[index]);
    }

    /* Format is the inverse of parse, which is what makes a reply to
     * $_event.origin reach its sender. */
    char buffer[128];
    const char *round_trip[] = { "#_internal", "#_parent", "#_scxml_42", "#_actor_door/front", "#_worker" };
    for (size_t index = 0; index < sizeof(round_trip) / sizeof(round_trip[0]); index += 1) {
        check(ta_scxml_target_parse(system, str(round_trip[index]), &target) == TA_OK,
              round_trip[index]);
        check(strcmp(format(system, target, buffer, sizeof(buffer)), round_trip[index]) == 0,
              round_trip[index]);
    }

    /* The absent target has no URI of its own. */
    uint32_t needed = 1;
    check(ta_scxml_target_format(system,
                                 &(ta_scxml_target_format_params){
                                     .target = { .kind = TA_SCXML_TARGET_SELF },
                                     .buffer = buffer, .capacity = sizeof(buffer), .needed = &needed }) ==
                  TA_ERR_NOT_FOUND &&
              needed == 0,
          "the absent target has no URI");

    /* A buffer that is too small reports what it needs. */
    needed = 0;
    check(ta_scxml_target_parse(system, str("#_scxml_42"), &target) == TA_OK, "#_scxml_42");
    check(ta_scxml_target_format(system, &(ta_scxml_target_format_params){ .target = target, .buffer = buffer,
                                                                          .capacity = 4, .needed = &needed }) ==
                  TA_ERR_NOMEM &&
              needed == strlen("#_scxml_42") + 1u,
          "a short buffer reports the length it needs");

    check(ta_scxml_target_parse(NULL, str("#_parent"), &target) == TA_ERR_INVALID, "a null system is invalid");
    check(ta_scxml_target_parse(system, str("#_parent"), NULL) == TA_ERR_INVALID, "a null out is invalid");

    ta_system_destroy(system);
    if (failures != 0) {
        printf("scxml uri test: %u check(s) failed\n", failures);
        return 1;
    }
    printf("scxml uri test passed\n");
    return 0;
}
