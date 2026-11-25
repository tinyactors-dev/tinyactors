# Mechanical C signature refactoring

Use ast-grep for structural matches and rewrites. Use the compiler to validate
the resulting translation units. Neither replaces the semantic mapping review.

## Before changing a signature

Record the function's qualified identity (path plus name for static functions),
old prototype, proposed prototype, argument-to-field mapping, outputs, fuel
semantics and callers. Include declarations, definitions, callback typedefs,
function-address uses, indirect calls and macro wrappers. Count immediate
parameters, not commas in text or nested function-pointer parameters.

Take the inventory from the current source after concurrent feature work lands.
Do not trust an earlier count as the baseline. Exclude third-party and generated
copies from the authored-code work queue; generated callable code still needs
enforcement through its generator. Treat parse failures as failures.

## ast-grep procedure

1. Inspect the C node kinds with the pinned ast-grep version. Use
   `ast-grep run --debug-query=ast -p '<representative declaration>' -l c`.
2. Match the exact symbol and expected old shape. Restrict paths for static names
   such as `check`, `convert` or `initialize`.
3. Preview matches and compare their count to the inventory. Distinguish calls
   from declarations and function-address expressions.
4. Prepare a rewrite with designated initializers. For example, replacing a
   known nine-argument call with a transfer and progress is safe only after the
   two output destinations and input expressions have been reviewed.
5. Apply only the reviewed paths and matches. Add structures/prototypes normally
   where structural rewriting would obscure intent. Avoid text-based global
   replacements of C declarations and calls.
6. Scan again. The old shapes must be absent, and all planned new shapes must
   exist. Verify a second application makes no changes.
7. Compile the affected dependency closure, including tests and callbacks that
   consume the interface. Repair misses with structural matches.

Do not copy an illustrative command into automation without confirming its
behavior against the installed version. Store reusable rewrite rules with valid
and invalid fixtures where the repository's tooling supports them.

## Semantics the compiler cannot prove

- Mapping equal-typed integers to the wrong fields still compiles.
- New temporaries can change evaluation sequencing or scope. Review calls with
  increments, allocations, mutation, function calls, macros or volatile access;
  do not duplicate expression evaluation. Existing defined ordering must remain.
- An argument struct can introduce aliasing with output/state/fuel. Validate it
  before mutation; local copies of required input fields may be needed.
- Compound literals live to the end of their containing block, not across a
  resumed fiber invocation. Do not retain their addresses.
- Preserve which outputs change on errors/yields and which fuel reservations
  can be consumed on allocation refusal. Do not zero outputs indiscriminately.
- Persistent continuation state keeps integer cursors/handles; rebuilding a
  call-scoped view on retry must not reset committed progress.
- Internal static names can collide in an amalgamation despite separate builds
  passing. Include one inexpensive amalgamation compile at a milestone.

## Verification budget

Follow the user's and repository's chosen validation scope. Prefer strict
compiler checks and exact structural checks per batch. Compile affected fuzz
harnesses when their interfaces change; do not infer permission to run fuzzing.
Use targeted existing behavioral checks for retry, aliasing, partial progress,
or callback changes. Do not weaken expectations or remove tests to save time.
Report what actually ran. Keep full-suite checks distinct from scoped evidence.
