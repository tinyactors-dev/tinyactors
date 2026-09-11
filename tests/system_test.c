#include "host.h"
#include "../src/internal.h"

static ta_status_t create_root(ta_test_allocator_t *host, ta_arena_t **out)
{
    ta_chunk_allocator_t allocator = ta_test_allocator_callbacks(host);
    ta_arena_options_t options = {512, 65536, (size_t)-1, 65536};
    return ta_arena_create(&allocator, &options, out);
}

static int registration_cases(void)
{
    ta_test_allocator_t host;
    ta_arena_t *root;
    ta_system_t *system;
    ta_system_config_t config;
    ta_memory_stats_t before, after;
    char mutable_type[] = "urn:test:ws";
    const char *aliases[] = { "ws", NULL };
    ta_ioprocessor_desc_t processors[] = { { .type = mutable_type, .aliases = aliases }, {0} };
    ta_service_desc_t services[] = { { .type = "urn:test:service", .event_processor = "ws" }, {0} };
    ta_test_allocator_init(&host, 0, 0);
    if (create_root(&host, &root) != TA_OK) return 1;
    ta_arena_stats(root, &before);
    ta_system_config_init(&config);
    config.session_prefix = ta_text_from_cstr("test");
    config.data_model = ta_text_from_cstr("bytes");
    config.ioprocessors = processors;
    config.services = services;
    if (ta_system_create(root, &config, &system) != TA_OK) return 2;
    mutable_type[9] = 'X';
    aliases[0] = "changed";
    if (system->processor_count != 2 || system->service_count != 2 ||
        tai_processor(system, ta_text_from_cstr("ws")) == NULL ||
        tai_processor(system, ta_text_from_cstr("urn:test:ws")) == NULL ||
        tai_processor(system, ta_text_from_cstr("changed")) != NULL) return 3;
    if (ta_system_destroy(system) != TA_OK) return 4;
    ta_arena_stats(root, &after);
    if (before.reserved_bytes != after.reserved_bytes || before.used_bytes != after.used_bytes ||
        before.descendant_count != after.descendant_count) return 5;
    aliases[0] = "scxml";
    if (ta_system_create(root, &config, &system) != TA_INVALID || system != NULL) return 6;
    aliases[0] = "ws";
    services[0].event_processor = "missing";
    if (ta_system_create(root, &config, &system) != TA_INVALID || system != NULL) return 7;
    services[0].event_processor = "ws";
    config.session_prefix = ta_text_from_cstr("bad prefix");
    if (ta_system_create(root, &config, &system) != TA_INVALID) return 8;
    ta_arena_close(root);
    if (!ta_test_allocator_releases_ok(&host)) return 9;
    ta_test_allocator_reset(&host);
    return 0;
}

static int creation_failures(void)
{
    size_t failure;
    unsigned successes = 0;
    for (failure = 1; failure < 60; ++failure) {
        ta_test_allocator_t host;
        ta_arena_t *root;
        ta_system_t *system;
        ta_status_t status;
        ta_system_config_t config;
        ta_memory_stats_t before, after;
        ta_test_allocator_init(&host, 0, 0);
        if (create_root(&host, &root) != TA_OK) return 10;
        ta_arena_stats(root, &before);
        host.fail_acquire = host.acquire_count + failure;
        ta_system_config_init(&config);
        config.session_prefix = ta_text_from_cstr("test");
        config.data_model = ta_text_from_cstr("bytes");
        config.ioprocessors = (const ta_ioprocessor_desc_t[]) {
            { .type = "urn:test:ws", .aliases = (const char *const[]) { "ws", NULL } }, {0}
        };
        status = ta_system_create(root, &config, &system);
        if (status == TA_OK) {
            ++successes;
            if (ta_system_destroy(system) != TA_OK) return 11;
        } else if (status != TA_NO_MEMORY && status != TA_LIMIT) return 12;
        ta_arena_stats(root, &after);
        if (before.reserved_bytes != after.reserved_bytes || before.used_bytes != after.used_bytes ||
            before.descendant_count != after.descendant_count) return 13;
        ta_arena_close(root);
        if (!ta_test_allocator_releases_ok(&host)) return 14;
        ta_test_allocator_reset(&host);
    }
    return successes == 0 ? 15 : 0;
}

int main(void)
{
    int result = registration_cases();
    return result == 0 ? creation_failures() : result;
}
