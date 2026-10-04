/* test: runtime asan */
/*
 * The SCXML target URI grammar (ADR 0044). Core owns it, so this is the one
 * place it is specified: every spelling of a `<send>` target and every URI
 * core writes for a chart goes through these two functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyactors.h"
#include "test_allocator.h"

static uint32_t failures;

static void check(bool condition, const char *what) {
    if (!condition) {
        printf("scxml uri test: %s\n", what);
        failures += 1;
    }
}

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
    ta_test_allocator heap = { 0 };
    ta_allocator allocator = ta_test_allocator_get(&heap);
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

    /* A named actor's logical address, interned (ADR 0065): the target holds
     * a reference to the same handle as every other use of that address. */
    check(ta_scxml_target_parse(system, str("#_actor_door/front"), &target) == TA_OK &&
              target.kind == TA_SCXML_TARGET_ACTOR && target.as.actor.kind == TA_ACTOR_REF_NAMED &&
              str_is(ta_address_view(target.as.actor.as.named).family.name, "door") &&
              str_is(ta_address_view(target.as.actor.as.named).name, "front"),
          "#_actor_door/front");
    ta_scxml_target again = {0};
    check(ta_scxml_target_parse(system, str("#_actor_door/front"), &again) == TA_OK &&
              again.as.actor.as.named == target.as.actor.as.named,
          "an address parsed twice is the same handle");
    ta_scxml_target_release(&again);
    ta_scxml_target_release(&target);

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

    /*
     * A session ID's text is <incarnation>.<session>.<family> (ADR 0088). This
     * system's incarnation is 0: its sessions parse to themselves, with the
     * family named; another incarnation's to no session, keeping the family.
     * The family comes last, so it may hold '.' and '/'.
     */
    ta_atom shop = 0;
    check(ta_system_intern_atom(system, str("shop.cart/invoke"), &shop) == TA_OK && shop != 0, "a family atom");
    check(ta_scxml_target_parse(system, str("#_scxml_0.42.shop.cart/invoke"), &target) == TA_OK &&
              target.as.actor.kind == TA_ACTOR_REF_RUNTIME && target.as.actor.as.runtime == 42 &&
              target.as.actor.family == shop,
          "#_scxml_0.42.<family>");
    check(ta_scxml_target_parse(system, str("#_scxml_9.42.shop.cart/invoke"), &target) == TA_OK &&
              target.as.actor.as.runtime == 0 && target.as.actor.family == shop,
          "another incarnation's session");
    check(ta_scxml_target_parse(system, str("#_scxml_42"), &target) == TA_OK && target.as.actor.family == 0,
          "a bare session ID names no family");
    const char *not_text[] = { "#_scxml_0.42.", "#_scxml_0.42", "#_scxml_x.42.door", "#_scxml_0..door" };
    for (size_t index = 0; index < sizeof(not_text) / sizeof(not_text[0]); index += 1) {
        check(ta_scxml_target_parse(system, str(not_text[index]), &target) == TA_OK &&
                  target.as.actor.as.runtime == 0 && target.as.actor.family == 0,
              not_text[index]);
    }
    char text[64];
    uint32_t text_needed = 0;
    check(ta_session_id_format(system, &(ta_session_id_format_params){ .session = 42, .family = str("door"),
                                                                      .buffer = text, .capacity = sizeof(text),
                                                                      .needed = &text_needed }) == TA_OK &&
              strcmp(text, "0.42.door") == 0 && text_needed == 10,
          "ta_session_id_format");
    check(ta_session_id_format(system, &(ta_session_id_format_params){ .session = 0, .family = str("door"),
                                                                      .buffer = text, .capacity = sizeof(text),
                                                                      .needed = &text_needed }) ==
              TA_ERR_INVALID,
          "session 0 has no text");
    check(ta_session_id_format(system, &(ta_session_id_format_params){ .session = 42, .family = str(""),
                                                                      .buffer = text, .capacity = sizeof(text),
                                                                      .needed = &text_needed }) ==
              TA_ERR_INVALID,
          "an empty family has no text");

    /*
     * ADR 0096: a family and a name are percent-encoded, so either may hold
     * '/' or '%'. Parse decodes %XX in either case of hex digit, and takes
     * any other byte but '/' as it is.
     */
    struct {
        const char *uri;
        const char *family;
        const char *name;
    } decoded[] = {
        { "#_actor_door/a%2Fb", "door", "a/b" },
        { "#_actor_sh%2Fop/x%25y", "sh/op", "x%y" },
        { "#_actor_door/a%2fb", "door", "a/b" },
        { "#_actor_door/%41", "door", "A" },
        { "#_actor_x/caf\xc3\xa9", "x", "caf\xc3\xa9" },
        { "#_actor_x/a b#c", "x", "a b#c" },
        { "#_actor_x/%2F", "x", "/" },
    };
    for (size_t index = 0; index < sizeof(decoded) / sizeof(decoded[0]); index += 1) {
        check(ta_scxml_target_parse(system, str(decoded[index].uri), &target) == TA_OK &&
                  target.kind == TA_SCXML_TARGET_ACTOR && target.as.actor.kind == TA_ACTOR_REF_NAMED &&
                  str_is(ta_address_view(target.as.actor.as.named).family.name, decoded[index].family) &&
                  str_is(ta_address_view(target.as.actor.as.named).name, decoded[index].name),
              decoded[index].uri);
        ta_scxml_target_release(&target);
    }
    /* An encoded and a hand-written spelling of one address are one handle. */
    check(ta_scxml_target_parse(system, str("#_actor_door/%66ront"), &target) == TA_OK &&
              ta_scxml_target_parse(system, str("#_actor_door/front"), &again) == TA_OK &&
              target.as.actor.as.named == again.as.actor.as.named,
          "%66ront is front");
    ta_scxml_target_release(&again);
    ta_scxml_target_release(&target);

    /* Format encodes '/', '%', and what a fragment may not hold; it keeps
     * the unreserved characters, the sub-delims, ':', '@' and '?'. */
    struct {
        const char *family;
        const char *name;
        const char *uri;
    } encoded[] = {
        { "door", "a/b", "#_actor_door/a%2Fb" },
        { "sh/op", "x%y", "#_actor_sh%2Fop/x%25y" },
        { "x", "caf\xc3\xa9", "#_actor_x/caf%C3%A9" },
        { "x", "a b#c\"<>\\^`{|}", "#_actor_x/a%20b%23c%22%3C%3E%5C%5E%60%7B%7C%7D" },
        { "x", "\x01\x7f", "#_actor_x/%01%7F" },
        { "x", "a:b@c?d!$&'()*+,;=-._~", "#_actor_x/a:b@c?d!$&'()*+,;=-._~" },
    };
    for (size_t index = 0; index < sizeof(encoded) / sizeof(encoded[0]); index += 1) {
        ta_address *named = NULL;
        ta_actor_address address = { .family = { .name = str(encoded[index].family) }, .name = str(encoded[index].name) };
        check(ta_address_intern(system, &address, &named) == TA_OK, encoded[index].uri);
        ta_scxml_target written = { .kind = TA_SCXML_TARGET_ACTOR,
                                    .as.actor = { .kind = TA_ACTOR_REF_NAMED, .as.named = named } };
        char uri[128];
        check(strcmp(format(system, written, uri, sizeof(uri)), encoded[index].uri) == 0, encoded[index].uri);
        /* ... and parse reads it back to the same address. */
        check(ta_scxml_target_parse(system, str(uri), &target) == TA_OK && target.as.actor.as.named == named,
              encoded[index].uri);
        ta_scxml_target_release(&target);
        ta_address_release(named);
    }

    /* ta_actor_uri_format writes the same text with no system or handle. */
    for (size_t index = 0; index < sizeof(encoded) / sizeof(encoded[0]); index += 1) {
        char uri[128];
        uint32_t uri_needed = 0;
        ta_actor_uri_format_params params = {
            .address = { .family = { .name = str(encoded[index].family) }, .name = str(encoded[index].name) },
            .buffer = uri, .capacity = sizeof(uri), .needed = &uri_needed };
        check(ta_actor_uri_format(&params) == TA_OK && strcmp(uri, encoded[index].uri) == 0 &&
                  uri_needed == strlen(encoded[index].uri) + 1u,
              encoded[index].uri);
    }
    uint32_t short_needed = 0;
    check(ta_actor_uri_format(&(ta_actor_uri_format_params){
              .address = { .family = { .name = str("door") }, .name = str("a/b") }, .buffer = NULL,
              .capacity = 0, .needed = &short_needed }) == TA_ERR_NOMEM &&
              short_needed == strlen("#_actor_door/a%2Fb") + 1u,
          "ta_actor_uri_format measures without a buffer");
    check(ta_actor_uri_format(&(ta_actor_uri_format_params){
              .address = { .family = { .name = str("") }, .name = str("x") }, .needed = &short_needed }) ==
              TA_ERR_INVALID,
          "an empty family has no URI");
    check(ta_actor_uri_format(&(ta_actor_uri_format_params){
              .address = { .family = { .name = str("x") }, .name = str("") }, .needed = &short_needed }) ==
              TA_ERR_INVALID,
          "an empty name has no URI");

    /* Anything else is an invalid target (SCXML 6.2, IRP test194), and so
     * is a malformed escape. */
    const char *invalid[] = { "elsewhere", "", "#_", "#_actor_", "#_actor_door", "#_actor_/front",
                              "#_actor_door/", "#_actor_a/b/c", "#_actor_door/a%", "#_actor_door/a%2",
                              "#_actor_door/a%zz", "#_actor_d%G1/x", "#_actor_door/%%41" };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index += 1) {
        check(ta_scxml_target_parse(system, str(invalid[index]), &target) == TA_ERR_INVALID,
              invalid[index][0] == '\0' ? "(empty)" : invalid[index]);
    }

    /* Format is the inverse of parse, which is what makes a reply to
     * $_event.origin reach its sender. */
    char buffer[128];
    const char *round_trip[] = { "#_internal", "#_parent", "#_scxml_42", "#_scxml_0.42.shop.cart/invoke",
                                 "#_actor_door/front", "#_actor_sh%2Fop/a%2Fb%25c", "#_worker" };
    for (size_t index = 0; index < sizeof(round_trip) / sizeof(round_trip[0]); index += 1) {
        check(ta_scxml_target_parse(system, str(round_trip[index]), &target) == TA_OK,
              round_trip[index]);
        check(strcmp(format(system, target, buffer, sizeof(buffer)), round_trip[index]) == 0,
              round_trip[index]);
        ta_scxml_target_release(&target);
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
