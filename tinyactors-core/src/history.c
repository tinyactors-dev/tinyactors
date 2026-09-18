#include "history.h"

uint64_t *ta_history_slot_words(const ta_history_state *history, uint32_t slot) {
    return (uint64_t *)((unsigned char *)history->storage + history->layout->slots[slot].offset);
}
