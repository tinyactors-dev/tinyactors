// A small API that uses every rule once.
// expect types.ta_item_kind.constants.TA_ITEM_BIG.symbol = "big"
// expect types.ta_item_kind.constants.TA_ITEM_SMALL.value = 1
// expect types.ta_box.kind = "handle"
// expect types.ta_box.created_by = [{"function": "ta_box_create", "path": ["params", "out"]}]
// expect types.ta_box.destroyed_by = [{"function": "ta_box_destroy", "path": ["box"]}]
// expect types.ta_box_params.fields.items.shape = "counted"
// expect types.ta_box_params.fields.count.count_of = "items"
// expect types.ta_box_params.fields.names.terminator = "ptr"
// expect types.ta_box_params.fields.label.optional = true
// expect types.ta_item.tagged.members = {"TA_ITEM_NONE": null, "TA_ITEM_SMALL": "small", "TA_ITEM_BIG": "big"}
// expect types.ta_list_params.fields.items.shape = "buffer"
// expect types.ta_list_params.fields.items.valid_on = "success"
// expect types.ta_list_params.fields.needed.valid_on = "always"
// expect types.ta_list_params.fields.capacity.capacity_of = "items"
// expect functions.ta_box_create.value_model = {"kind": "any"}
// expect functions.ta_box_put.value_model = {"kind": "actor", "path": ["actor"]}
// expect functions.ta_box_list.returns.success = "TA_OK"
// expect functions.ta_box_get.params.out.release = "ta_value_release"
// expect constants.TA_ITEM_NOTHING.type = {"type": "ta_item_id"}
#include <stdint.h>
#include <stdbool.h>

typedef int32_t ta_result;
enum TA_CONSTANTS_OF(ta_result, "TA_") { TA_OK TA_SUCCESS = 0, TA_ERR = -1 };
typedef uint32_t ta_item_id;
typedef uint64_t ta_actor_id;
#define TA_ITEM_NOTHING ((ta_item_id)UINT32_MAX)
typedef struct { const char *ptr; uint32_t len; } ta_str TA_STRING;
typedef struct ta_value ta_value TA_HOST_VALUE;
struct ta_value { void *self; void *v; };
typedef struct ta_box ta_box TA_HANDLE;
typedef enum { TA_ITEM_NONE = 0, TA_ITEM_SMALL, TA_ITEM_BIG } ta_item_kind TA_SYMBOLS("TA_ITEM_");
typedef struct {
    ta_item_kind kind;
    union {
        TA_WHEN(TA_ITEM_SMALL) uint32_t small;
        TA_WHEN(TA_ITEM_BIG) ta_str big;
    } as;
} ta_item TA_TAGGED(kind, as);
typedef struct {
    TA_COUNTED_BY(count) const ta_item *items;
    uint32_t count;
    TA_TERMINATED_BY(ptr) TA_OPTIONAL const ta_str *names;
    TA_IN TA_OPTIONAL const ta_str *label;
    ta_value seed;
    ta_box **out;
} ta_box_params;
typedef struct {
    TA_BUFFER(capacity, needed) TA_OPTIONAL ta_item_id *items;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *needed;
} ta_list_params;

static inline void ta_value_release(TA_INOUT TA_OPTIONAL ta_value *value) TA_ANY_VALUE_MODEL { (void)value; }
TA_API ta_result ta_box_create(TA_IN const ta_box_params *params) TA_ANY_VALUE_MODEL;
TA_API void ta_box_destroy(TA_DESTROYS ta_box *box);
TA_API ta_result ta_box_put(ta_box *box, ta_actor_id actor, TA_IN const ta_value *value) TA_VALUE_MODEL(actor, actor);
TA_API ta_result ta_box_get(ta_box *box, TA_OUT TA_RELEASE(ta_value_release) ta_value *out);
TA_API ta_result ta_box_list(const ta_box *box, TA_IN const ta_list_params *params);
