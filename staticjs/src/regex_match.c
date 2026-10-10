/*
 * Regular expressions as values (docs/regex.md, section 7): their
 * properties.
 */
#include "vm.h"
#include "regex_program.h"

static sjs_result regex_fault(sjs_vm *vm, const char *message, sjs_str subject) {
    sjs_vm_fault(vm, message);
    sjs_error_subject(&vm->error, subject);
    return SJS_ERR_PROGRAM;
}

/* RegExp.prototype's getters of the flags, by name. */
static const struct {
    const char *name;
    uint32_t flag;
} FLAG_GETTERS[] = {
    { "hasIndices", SJS_REGEX_HAS_INDICES }, { "global", SJS_REGEX_GLOBAL },
    { "ignoreCase", SJS_REGEX_IGNORE_CASE }, { "multiline", SJS_REGEX_MULTILINE },
    { "dotAll", SJS_REGEX_DOT_ALL },         { "unicode", SJS_REGEX_UNICODE },
    { "unicodeSets", SJS_REGEX_UNICODE_SETS }, { "sticky", SJS_REGEX_STICKY },
};

static sjs_result new_text(sjs_vm *vm, sjs_str text, sjs_value *out) {
    sjs_string *s;
    sjs_result r = sjs_string_make(vm->context, text, &s);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    *out = sjs_heap_value(SJS_STRING, &s->h);
    return SJS_OK;
}

sjs_result sjs_regex_property(sjs_vm *vm, sjs_member member, sjs_value *out) {
    const sjs_regexp *re = SJS_AS_REGEXP(member.object);
    sjs_str name = sjs_string_view(member.key);
    *out = sjs_undefined();
    if (sjs_str_equal(name, SJS_STR("source"))) return new_text(vm, sjs_regex_source(&re->h), out);
    if (sjs_str_equal(name, SJS_STR("flags"))) {
        char flags[8];
        return new_text(vm, (sjs_str){ flags, sjs_regex_flag_text(re->flags, flags) }, out);
    }
    if (sjs_str_equal(name, SJS_STR("lastIndex"))) {
        *out = sjs_num(0);
        return SJS_OK;
    }
    for (uint32_t i = 0; i < sizeof FLAG_GETTERS / sizeof FLAG_GETTERS[0]; i++) {
        if (sjs_str_equal(name, sjs_cstr(FLAG_GETTERS[i].name))) {
            *out = sjs_boolean((re->flags & FLAG_GETTERS[i].flag) != 0);
            return SJS_OK;
        }
    }
    const sjs_name *n = sjs_name_of(member.key);
    if (n != NULL && (n->flags & SJS_NAME_ON_REGEXP) != 0) {
        return regex_fault(vm, "staticjs calls methods; it does not read them as values", name);
    }
    return SJS_OK;
}
