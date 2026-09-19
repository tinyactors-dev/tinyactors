// expect error: ta_system: no function destroys this handle; mark its destroy parameter TA_DESTROYS
// expect error: ta_orphan: no function creates this handle
typedef struct ta_system ta_system TA_HANDLE;
typedef struct ta_orphan ta_orphan TA_HANDLE;
TA_API int ta_system_create(ta_system **out);
TA_API void ta_orphan_destroy(TA_DESTROYS ta_orphan *orphan);
