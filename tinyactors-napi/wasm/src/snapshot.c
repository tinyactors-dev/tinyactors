/* readSnapshot, as the addon's src/snapshot.c: a snapshot read without a
 * host, over core's snapshot reader (tinyactors-core/docs/snapshot-requests.md).
 * The reader is one block of the environment's allocator, its size first:
 * ta_host_snapshot_reader_open copies the pack and reads each live
 * segment's region into it. JavaScript holds the block's address and frees
 * it (ta_web_snapshot_free) when its Snapshot is collected. A request's
 * message, its working memory and its reply are the environment's arena,
 * which JavaScript reads before its call leaves. */
#include "web.h"

#include <string.h>

/* A reply's room on the first try; a larger one is asked for again with
 * the length the reader gave. */
static const size_t ta_web_snapshot_reply_room = 16384;

/* The block: its size, then the reader, 8-aligned. */
typedef struct ta_web_snapshot_block {
    size_t bytes;
    uint64_t pad;
} ta_web_snapshot_block;

static const ta_host_snapshot_reader *ta_web_snapshot_reader(ta_web_snapshot_block *block) {
    return (const ta_host_snapshot_reader *)(const void *)(block + 1);
}

/* Record: 0-1 the pack's bytes (in the call's arena). Answers the status;
 * the record: 0 the block's address, 1-2 its system id, 3-4 its
 * datamodel's label, 5-6 its time, 7 its segments, 8-9 the datamodels'
 * state, views into the pack the call was given. */
TA_WEB_EXPORT int32_t ta_web_snapshot_read(ta_web_env *env) {
    uint32_t *word = env->record.word;
    ta_bytes pack = ta_web_bytes_at(word, 0);
    ta_host_snapshot_contents contents;
    contents.size = sizeof contents;
    size_t size = ta_host_snapshot_reader_size(pack);
    if (!size || ta_host_snapshot_unpack(pack, &contents) != TA_OK) return TA_ERROR_INVALID;
    ta_allocator *allocator = &env->allocator;
    size_t bytes = sizeof(ta_web_snapshot_block) + size;
    ta_web_snapshot_block *block = (ta_web_snapshot_block *)allocator->alloc(allocator->context, bytes, 8);
    if (!block) return TA_ERROR_NO_MEMORY;
    block->bytes = bytes;
    const ta_host_snapshot_reader *reader = NULL;
    ta_host_snapshot_reader_input input = {sizeof input, 0, pack, {block + 1, size, 0}};
    ta_status status = ta_host_snapshot_reader_open(&input, &reader);
    if (status != TA_OK) {
        allocator->free(allocator->context, block, bytes);
        return status;
    }
    word[0] = ta_web_address(block);
    ta_web_set_text(word, 1, contents.system_id);
    ta_web_set_text(word, 3, contents.datamodel);
    ta_web_set_word64(word, 5, contents.time);
    word[7] = contents.image_count;
    ta_web_set_bytes(word, 8, (ta_bytes){contents.state, contents.state_length});
    return TA_OK;
}

/* Record: 0 a block's address. */
TA_WEB_EXPORT void ta_web_snapshot_free(ta_web_env *env) {
    ta_web_snapshot_block *block = (ta_web_snapshot_block *)(uintptr_t)env->record.word[0];
    if (block) env->allocator.free(env->allocator.context, block, block->bytes);
}

/* Record: 0 a block's address, 1-2 a request's message (in the call's
 * arena). Answers the status; the record: 0-1 the reply, in the arena. */
TA_WEB_EXPORT int32_t ta_web_snapshot_request(ta_web_env *env) {
    uint32_t *word = env->record.word;
    ta_web_snapshot_block *block = (ta_web_snapshot_block *)(uintptr_t)word[0];
    if (!block) return TA_ERROR_INVALID;
    ta_host_snapshot_request request = {sizeof request, 0, ta_web_bytes_at(word, 1), &env->scratch.view};
    ta_buffer reply = {NULL, ta_web_snapshot_reply_room, 0};
    ta_status status = TA_ERROR_NO_MEMORY;
    for (int tries = 0; tries < 2; tries++) {
        reply.pointer = ta_growable_arena_alloc(&env->scratch.arena, reply.capacity, 8);
        if (!reply.pointer) return TA_ERROR_NO_MEMORY;
        status = ta_host_snapshot_reader_request(ta_web_snapshot_reader(block), &request, &reply);
        if (status != TA_ERROR_NO_MEMORY || reply.length <= reply.capacity) break;
        reply.capacity = reply.length; /* the room it needs, for the second try */
    }
    if (status == TA_OK) ta_web_set_bytes(word, 0, (ta_bytes){reply.pointer, reply.length});
    return status;
}
