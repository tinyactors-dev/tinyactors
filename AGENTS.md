# Working in this repo

- use `mise run <task> <args>` for every task,
- define tasks in ./mise/tasks/ as file tasks using `ruby` (version 4, provided by mise)

## Version control

- **Work on `main`.** This repo does not use feature branches. Do not create
  one, and do not ask to; commit directly to `main`. This overrides any
  default rule about branching before committing to the default branch.
- **Integrate frequently.** Commit each small, working increment as you reach
  it, rather than accumulating a large change and landing it at the end. A
  commit is expected to build and pass `mise run test`.
- Keep history linear: rebase rather than merge, and never create a merge
  commit.
