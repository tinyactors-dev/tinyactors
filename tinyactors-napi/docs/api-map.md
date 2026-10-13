# The mapping table: the JavaScript API and core

Each item of `@tinyactors/node`'s API next to what core has behind it
(tinyactors-core/docs/bindings.md, section 2: "An item with no core
function behind it does not belong"). Written before the item, in the
same commit, and kept in step with `index.d.ts`.

**The rule:** every row's Core cell names, in backticks, at least one of

- a function of core's API descriptions
  (`tinyactors-core/include/*.api.json`): `ta_host_send`;
- a constant of them: an effect (`TA_EFFECT_LOG`, `TA_HOST_EFFECT_PEER`),
  a run status (`TA_RUN_IDLE`), a flag;
- a request method of `tinyactors-core/docs/requests.md`: `actor.context`;

and nothing else in backticks. `mise run lint` checks it
(`mise/lib/api_map.rb` at the repository root): a name core does not have
fails. Say in Notes how the item uses them: the turn, a request, the
handling of an effect.

A core function no item uses is listed below the table: used internally
(say where) or not exposed (say why).

A row marked *planned* is an item the plan (ROADMAP.md) calls for,
written ahead so its core is checked and the API review sees it; the
commit that adds the item removes the mark.

| API item | Core | Notes |
|---|---|---|
