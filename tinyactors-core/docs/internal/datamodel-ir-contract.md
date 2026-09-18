# Compiled IR contract for data-model operations

The definition compiler reduces frontend constructs to a small semantic IR. The data model sees IDs and values, never XML nodes.

## Suggested data-related IR operations

```text
DECLARE_DATA       name
INIT_DATA_EXPR     name, expr_id
INIT_DATA_LITERAL  name, literal_id
SET_DATA_VALUE     name, prepared_value

EVAL_COND          expr_id
EVAL_VALUE         expr_id
READ_LOCATION      location_expr_id
ASSIGN_EXPR        location_expr_id, value_expr_id
ASSIGN_LITERAL     location_expr_id, literal_id

SCRIPT             script_expr_id

FOREACH_BEGIN      collection_expr_id, item_name, optional_index_name
FOREACH_NEXT
FOREACH_END

BUILD_PARAMS_BEGIN
PARAM_EXPR         name, value_expr_id
PARAM_LOCATION     name, location_expr_id
BUILD_PARAMS_END

CONTENT_EXPR       value_expr_id
CONTENT_LITERAL    literal_id
```

The exact bytecode layout is internal. The important property is that these operations describe semantics rather than source syntax.

## Frontend mappings

A future SCXML importer can map:

- `<data id="x" expr="...">` -> declaration + expression initializer;
- `<data id="x">literal</data>` -> declaration + literal initializer;
- `<assign location="..." expr="...">` -> expression evaluation + assign;
- `<foreach array="..." item="x">` -> foreach begin/body/next;
- `<param name="x" expr="...">` -> named expression parameter;
- `<content expr="...">` -> content expression;
- `<script>` -> script action.
- `<scxml><script>` -> a one-action script block run once per session, after
  early data initialization and before the root initial transition.

A C builder can generate the same IR directly with no XML representation at any stage.

## Pausability

Any IR instruction that calls host/plugin code is a valid execution-budget boundary after the callback returns. The continuation records the next IR instruction plus any live temporary `ta_value`s.

A callback itself is synchronous from the engine's perspective. Host asynchronous work is modeled by later events, not by an unfinished data-model callback.
