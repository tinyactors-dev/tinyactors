/*
 * The send ID text grammar (ADR 0048). Core owns it, so this is the one
 * place it is specified: the text a chart sees through `idlocation`,
 * `_event.sendid` and `<cancel sendidexpr>`, and the `ta_send_id` a host
 * holds, are two forms of one ID, and the two functions are inverses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyactors.h"
#include "test_allocator.h"

static uint32_t failures;

static void check(bool condition, const char *what) {
    if (!condition) {
        printf("send id test: %s\n", what);
        failures += 1;
    }
}

static ta_str str(const char *text) {
    return (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) };
}

/* The text of an ID, or "" when it has none. */
static const char *format(const ta_system *system, ta_send_id id, char *buffer, uint32_t capacity) {
    uint32_t needed = 0;
    ta_result result = ta_send_id_format(
        system, &(ta_send_id_format_params){ .send_id = id, .buffer = buffer,
                                             .capacity = capacity, .needed = &needed });
    if (result != TA_OK) {
        buffer[0] = '\0';
    }
    return buffer;
}

/* parse(text) -> format -> the same text. */
static void round_trips(const ta_system *system, const char *text) {
    ta_send_id id = 0;
    char buffer[64];
    if (ta_send_id_parse(system, str(text), &id) != TA_OK) {
        printf("send id test: %s does not parse\n", text);
        failures += 1;
        return;
    }
    check(id != 0, "a parsed ID is not the absent one");
    check(strcmp(format(system, id, buffer, sizeof(buffer)), text) == 0, text);
}

int main(void) {
    ta_system *system = NULL;
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    if (ta_system_create(&(ta_system_config){ .allocator = allocator, .initial_time = 0 }, &system) != TA_OK) {
        printf("send id test: could not create a system\n");
        return 1;
    }

    char buffer[64];

    /* A generated ID: the form core writes for a <send> without an author ID.
     * It parses whether or not such a send exists, as a session URI does. */
    round_trips(system, "#_send.1");
    round_trips(system, "#_send.4294967296");

    /* A literal ID: the author's own text, once the system has interned it.
     * ta_system_intern_atom is how a host gets it in there; core interns the
     * literal IDs of every chart it compiles. */
    ta_atom timeout = 0;
    check(ta_system_intern_atom(system, str("timeout"), &timeout) == TA_OK, "intern the author's ID");
    round_trips(system, "timeout");

    /* Text that names no send ID of this system. */
    ta_send_id parsed = 12345;
    check(ta_send_id_parse(system, str("never-interned"), &parsed) == TA_ERR_NOT_FOUND,
          "an ID no chart of this system wrote is not found");
    check(parsed == 12345, "a failed parse leaves its output alone");

    /* `#_send.0` is not a generated ID: core's counter starts at one, so this
     * falls back to a literal ID, and no chart interned that text. */
    check(ta_send_id_parse(system, str("#_send.0"), &parsed) == TA_ERR_NOT_FOUND, "#_send.0 names no send");
    /* Nor is a prefix with no number, or with something that is not one. */
    check(ta_send_id_parse(system, str("#_send."), &parsed) == TA_ERR_NOT_FOUND, "#_send. names no send");
    check(ta_send_id_parse(system, str("#_send.x"), &parsed) == TA_ERR_NOT_FOUND, "#_send.x names no send");

    /* No text at all is not a send ID. */
    check(ta_send_id_parse(system, str(""), &parsed) == TA_ERR_INVALID, "empty text is invalid");
    check(ta_send_id_parse(system, str("#_send.1"), NULL) == TA_ERR_INVALID, "a null output is invalid");

    /* A send with no ID has no text. */
    uint32_t needed = 99;
    check(ta_send_id_format(system, &(ta_send_id_format_params){ .send_id = 0, .buffer = buffer,
                                                                 .capacity = sizeof(buffer),
                                                                 .needed = &needed }) == TA_ERR_NOT_FOUND,
          "the absent send ID has no text");
    check(needed == 0, "and needs no buffer");

    /* The size-query convention: a zero capacity reports what it needs. */
    ta_send_id generated = 0;
    check(ta_send_id_parse(system, str("#_send.1"), &generated) == TA_OK, "parse the generated ID");
    needed = 0;
    check(ta_send_id_format(system, &(ta_send_id_format_params){ .send_id = generated,
                                                                 .needed = &needed }) == TA_ERR_NOMEM,
          "a zero capacity is a size query");
    check(needed == strlen("#_send.1") + 1u, "which reports the text and its NUL");

    /* A too-small buffer reports the same, and writes nothing a caller could
     * mistake for the whole text. */
    needed = 0;
    check(ta_send_id_format(system, &(ta_send_id_format_params){ .send_id = generated, .buffer = buffer,
                                                                 .capacity = 3,
                                                                 .needed = &needed }) == TA_ERR_NOMEM,
          "a too-small buffer is not enough");
    check(needed == strlen("#_send.1") + 1u, "and reports the size it needed");

    /* Two different IDs never share text, and one ID always writes its own. */
    ta_send_id literal = 0;
    check(ta_send_id_parse(system, str("timeout"), &literal) == TA_OK, "parse the literal ID");
    check(literal != generated, "a literal ID is not the generated one");
    check(strcmp(format(system, literal, buffer, sizeof(buffer)), "timeout") == 0, "the literal's text");

    /* Interning: text another process wrote, which this system has not seen
     * (kernel ADR 0002), becomes the ID parsing then finds. */
    ta_send_id interned = 0;
    check(ta_send_id_intern(system, str("from-a-log"), &interned) == TA_OK, "intern an unseen literal");
    check(interned != 0, "an interned ID is not the absent one");
    check(strcmp(format(system, interned, buffer, sizeof(buffer)), "from-a-log") == 0, "its text");
    ta_send_id again = 0;
    check(ta_send_id_parse(system, str("from-a-log"), &again) == TA_OK && again == interned, "parse finds it after");
    check(ta_send_id_intern(system, str("#_send.7"), &again) == TA_OK &&
              strcmp(format(system, again, buffer, sizeof(buffer)), "#_send.7") == 0,
          "a generated ID interns to itself");
    check(ta_send_id_intern(system, str(""), &again) == TA_ERR_INVALID, "empty text does not intern");

    /* Invocation IDs: interning is the inverse of their text. */
    ta_invoke_id invoke = TA_INVOKE_ID_NONE;
    ta_str invoke_text = { 0 };
    check(ta_system_intern_invoke_id(system, str("state.9"), &invoke) == TA_OK, "intern an invoke ID");
    check(ta_system_invoke_id_text(system, invoke, &invoke_text) == TA_OK && invoke_text.len == 7 &&
              memcmp(invoke_text.ptr, "state.9", 7) == 0,
          "its text");
    ta_invoke_id same = TA_INVOKE_ID_NONE;
    check(ta_system_intern_invoke_id(system, str("state.9"), &same) == TA_OK && same == invoke, "the same text, the same ID");
    check(ta_system_intern_invoke_id(system, str(""), &same) == TA_ERR_INVALID, "empty text is no invoke ID");

    ta_system_destroy(system);
    check(test_allocator_finish(&heap) == 0, "every block came back, as it was allocated");

    if (failures != 0) {
        printf("send id test: %u checks failed\n", failures);
        return 1;
    }
    printf("send id test passed\n");
    return 0;
}
