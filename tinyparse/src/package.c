/*
** package.c -- the grammar-package manifest reader.
**
** A package is a directory holding a package.scm manifest beside a grammar.scm
** source. The hosted shell (cli.c) finds those files; this freestanding module
** parses the manifest bytes through the .scm reader and binds the manifest's
** params: onto the engine as compile-time parameters, so the package's grammar
** sees them when it is compiled. It surfaces the manifest's name:, version:, and
** extends: to the caller. Freestanding, like the rest of src/ except cli.c: it
** depends only on internal.h and the engine's public param seam.
*/
#include "internal.h"

/* ** A parsed (package ...) manifest, surfaced to the hosted shell. name and
** version are the declared identity; extends is the base package name the
** manifest declares with extends: (empty when it declares none). The params:
** map is not surfaced: pocketparse_apply_manifest binds each entry straight onto
** the engine as a compile-time parameter, since that is all a consumer needs. ok
** is 1 when the bytes held a well-formed (package ...) form. This struct names
** only public types, so cli.c can mirror it without pulling in internal.h.
** KEEP IN SYNC with the matching declaration in cli.c. */
typedef struct pocketparse_manifest {
    pocketparse_string name;
    pocketparse_string version;
    pocketparse_string extends;
    int                ok;
} pocketparse_manifest;

/* ** Decide the truthiness of a params: value form. A bytes form is the reader's
** shape for a `#x' literal -- and for the booleans #t / #f, which decode to no
** bytes, so they are told apart by the source byte just after the `#' (using the
** form's recorded start offset): `t' is true, `f' is false. An integer is true
** when nonzero; a string or symbol is true when nonempty; a #x byte literal is
** true when it carries any bytes. */
static int pp_manifest_value_truthy(const pp_form *value,
                                    pocketparse_string manifest_bytes)
{
    switch (value->kind) {
    case PP_FORM_BYTES: {
        uint32_t marker = value->start_byte + 1; /* the char after '#' */
        if (marker < manifest_bytes.length) {
            uint8_t c = manifest_bytes.pointer[marker];
            if (c == 't' || c == 'T')
                return 1;
            if (c == 'f' || c == 'F')
                return 0;
        }
        return value->text.length > 0;
    }
    case PP_FORM_INTEGER:
        return value->integer != 0;
    case PP_FORM_STRING:
    case PP_FORM_SYMBOL:
    case PP_FORM_KEYWORD:
        return value->text.length > 0;
    case PP_FORM_LIST:
    default:
        return value->count > 0;
    }
}

/* ** Bind each entry of a params: list onto the engine. The list alternates a
** keyword (the parameter name, colon already stripped by the reader) and its
** value form, e.g. (feature: #t timeout: 30). A trailing keyword with no value
** is bound truthy. Binding uses the public pocketparse_set_param path, so these
** become compile-time parameters the package's grammar resolves through
** (when PARAM: X). */
static pocketparse_result pp_manifest_bind_params(pocketparse_engine *engine,
                                                  const pp_form *params,
                                                  pocketparse_string manifest_bytes)
{
    uint32_t i;
    for (i = 0; i < params->count; i++) {
        const pp_form *key = params->items[i];
        int value = 1;
        if (key->kind != PP_FORM_KEYWORD)
            continue;
        if (i + 1 < params->count) {
            value = pp_manifest_value_truthy(params->items[i + 1], manifest_bytes);
            i++;
        }
        if (pocketparse_set_param(engine, key->text, value) != POCKETPARSE_OK)
            return POCKETPARSE_INVALID;
    }
    return POCKETPARSE_OK;
}

/* ** Parse a package.scm manifest and bind its params: onto the engine as
** compile-time parameters (the pocketparse_set_param path), so a later
** (when PARAM: X) in the package's grammar sees them. Each params: entry is a
** keyword: value pair; a #t / nonzero / nonempty value sets the parameter
** truthy, #f / 0 / empty clears it. The manifest's name:, version:, and
** extends: are written to *out. Returns POCKETPARSE_OK on a well-formed
** manifest, or POCKETPARSE_INVALID with a fault set on the engine otherwise.
** Binding happens BEFORE the CLI applies its own --param overrides, so the
** command line wins over a manifest default. The manifest is a single top-level
** (package ...) form
** whose body is a flat sequence of keyword/value pairs: name:, version:,
** params:, provides:, extends:. Unknown keywords are ignored, so the manifest
** can carry fields this resolver does not yet read. */
pocketparse_result pocketparse_apply_manifest(pocketparse_engine *engine,
                                              pocketparse_source manifest,
                                              pocketparse_manifest *out)
{
    pp_read_result read;
    const pp_form *package = 0;
    uint32_t       i;

    if (engine == 0 || out == 0)
        return POCKETPARSE_INVALID;
    out->name    = pp_text("");
    out->version = pp_text("");
    out->extends = pp_text("");
    out->ok      = 0;

    read = pp_read(engine->arena, manifest.bytes);
    if (!read.ok) {
        engine->fault = pp_text("package manifest is not valid .scm");
        return POCKETPARSE_INVALID;
    }

    for (i = 0; i < read.count; i++) {
        const pp_form *form = read.forms[i];
        if (form->kind == PP_FORM_LIST && form->count >= 1 &&
            form->items[0]->kind == PP_FORM_SYMBOL &&
            pp_string_equal_c(form->items[0]->text, "package")) {
            package = form;
            break;
        }
    }
    if (package == 0) {
        engine->fault = pp_text("package manifest has no (package ...) form");
        return POCKETPARSE_INVALID;
    }

    /* Walk the keyword/value pairs in the (package ...) body. */
    for (i = 1; i < package->count; i++) {
        const pp_form *key = package->items[i];
        const pp_form *value;
        if (key->kind != PP_FORM_KEYWORD)
            continue;
        if (i + 1 >= package->count)
            break;
        value = package->items[++i];

        if (pp_string_equal_c(key->text, "name")) {
            out->name = value->text;
        } else if (pp_string_equal_c(key->text, "version")) {
            out->version = value->text;
        } else if (pp_string_equal_c(key->text, "extends")) {
            out->extends = value->text;
        } else if (pp_string_equal_c(key->text, "params")) {
            if (value->kind == PP_FORM_LIST &&
                pp_manifest_bind_params(engine, value, manifest.bytes)
                    != POCKETPARSE_OK)
                return POCKETPARSE_INVALID;
        }
        /* provides: and unknown keywords are not consumed by the resolver. */
    }

    out->ok = 1;
    return POCKETPARSE_OK;
}
