// expect error: ta_ref.as.named: union members need TA_WHEN(constant)
// expect error: ta_bare.as: a union needs a TA_TAGGED(tag, union) owner
#include <stdint.h>
typedef enum { TA_REF_RUNTIME = 0, TA_REF_NAMED } ta_ref_kind TA_SYMBOLS("TA_REF_");
typedef struct { ta_ref_kind kind; union { TA_WHEN(TA_REF_RUNTIME) uint64_t runtime; uint32_t named; } as; } ta_ref TA_TAGGED(kind, as);
typedef struct { union { uint32_t a; uint64_t b; } as; } ta_bare;
