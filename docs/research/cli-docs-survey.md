# How command-line tools document themselves: nine docs sites, read 2026-11-26

What the documentation websites of nine open-source command-line tools
do with their information architecture: where the command reference sits,
what a landing page holds, how commands are grouped, what a command's page
shows, how guides are named. Read to compare with www.tinyactors.dev/cli/.
Every claim cites the page read; "inferred" marks what a page did not say.
The earlier survey behind [the CLI reference proposal](../proposals/cli-reference.md)
covered the wording of reference pages (summaries, captions); this one
covers their structure.

## Per tool

**GitHub CLI.** The Manual's sidebar: "Getting started", "gh", then every
command as a nested tree; its landing is about 300 words (links to
commands, examples and extensions; installation, configuration, GitHub
Enterprise, support) ([manual](https://cli.github.com/manual/)). The index
is flat and alphabetical, a page per command and subcommand at
`/manual/gh_<cmd>_<sub>`. A command's page: usage, Options, "Options
inherited from parent commands", aliases, Examples (commands only), See
also (the parent), "In use" (sessions with output)
([gh pr create](https://cli.github.com/manual/gh_pr_create)). Guides: one
"Examples" page with task headings, nearly every example with output
([examples](https://cli.github.com/manual/examples)). Labels are short
nouns. Generated from cobra's help (inferred).

**Cargo.** The Cargo Book: Getting Started, Cargo Guide, Cargo Reference,
Cargo Commands, FAQ ([book](https://doc.rust-lang.org/cargo/)). The
commands index lists categories: General, Build, Manifest, Package,
Publishing, Report, Deprecated and Removed; a page per command at
`commands/cargo-<name>.html` ([commands](https://doc.rust-lang.org/cargo/commands/index.html)).
A command's page is a man page: NAME, SYNOPSIS, DESCRIPTION, OPTIONS
(grouped, with "Common Options" in full on every page), ENVIRONMENT, EXIT
STATUS, EXAMPLES (commands only), SEE ALSO; prev/next links and search
([cargo-build](https://doc.rust-lang.org/cargo/commands/cargo-build.html)).
Guide chapters are topic nouns and show real output
([dependencies](https://doc.rust-lang.org/cargo/guide/dependencies.html)).

**uv.** Tabs: Introduction, Getting started, Guides, Concepts, Reference;
"Commands" under Reference ([docs](https://docs.astral.sh/uv/)). The
landing (about 500 words) shows an example session per feature, each
linking its guide. The reference is one page of every command, global
options repeated under each, no examples
([cli](https://docs.astral.sh/uv/reference/cli/)). Guides are gerunds
("Using tools") ending with "Next steps" to the reference
([tools](https://docs.astral.sh/uv/guides/tools/)).

**mise.** The sidebar ends with "CLI reference": "CLI overview", then one
entry per command labelled "mise <cmd>"
([getting started](https://mise.jdx.dev/getting-started.html)). `/cli/`
lists global flags, then the commands in eight groups named by job
("Install and inspect tools", "Shell and environment", "Tasks and project
automation", ...); a page per command at `/cli/<cmd>.html`
([cli](https://mise.jdx.dev/cli/)). A command's page: Arguments, Flags,
Examples (commands only), "Related documentation"; global flags linked,
not repeated ([use](https://mise.jdx.dev/cli/use.html)). Guides are topic
nouns ([continuous integration](https://mise.jdx.dev/continuous-integration.html)).

**Deno.** Sidebar: Get started, Guides, Concepts, Diagnostics, Advanced,
Reference; "CLI" under Reference lists every command
([runtime](https://docs.deno.com/runtime/)). The index groups commands as
Execution, Dependency management, Tooling, Other
([cli](https://docs.deno.com/runtime/reference/cli/)). A command's page
starts with hand-written task sections, options last; breadcrumb,
prev/next, sidebar, "On this page"
([deno lint](https://docs.deno.com/runtime/reference/cli/lint/)). Guides
are topic names and link the command's page
([linting and formatting](https://docs.deno.com/runtime/fundamentals/linting_and_formatting/)).

**git.** The Reference page is an index of about 115 links grouped by
job: Setup and Config, Getting and Creating Projects, Basic Snapshotting,
Branching and Merging, ..., Plumbing Commands; a command may sit in
several groups (diff in three) ([docs](https://git-scm.com/docs)). A
command's page is a man page with a version selector
([git-diff](https://git-scm.com/docs/git-diff)). Guides are man pages too
([giteveryday](https://git-scm.com/docs/giteveryday)); a Cheat Sheet is
linked as the quick reference.

**Terraform.** The sidebar is task-phrased ("Provisioning
Infrastructure", "Inspecting Infrastructure", ...), command pages nested
under their task, plus "Alphabetical List of Commands"
([cli](https://developer.hashicorp.com/terraform/cli)). The overview shows
the captured output of `terraform` with no arguments
([commands](https://developer.hashicorp.com/terraform/cli/commands)). A
command's page: usage, options, "Understand plan output" (sample output
in prose), examples (commands only)
([plan](https://developer.hashicorp.com/terraform/cli/commands/plan)).

**ripgrep.** No docs site: GUIDE.md (topic sections, examples with
output), FAQ.md (question headings), and `rg --help` and the man page as
the reference ([repository](https://github.com/BurntSushi/ripgrep),
[guide](https://github.com/BurntSushi/ripgrep/blob/master/GUIDE.md),
[FAQ](https://github.com/BurntSushi/ripgrep/blob/master/FAQ.md)).

**kubectl.** Reference holds "Command line tool (kubectl)": an
introduction, a Quick Reference, the generated reference and more
([kubectl](https://kubernetes.io/docs/reference/kubectl/)). The generated
reference is flat and alphabetical, labelled "(auto-generated page)"
([generated](https://kubernetes.io/docs/reference/kubectl/generated/),
[kubectl apply](https://kubernetes.io/docs/reference/kubectl/generated/kubectl_apply/)).
Tasks have imperative titles and show expected output
([port forward](https://kubernetes.io/docs/tasks/access-application-cluster/port-forward-access-application-cluster/)).
The Quick Reference is task sections of one-line commands with `#`
comments ([quick reference](https://kubernetes.io/docs/reference/kubectl/quick-reference/)).

## Common patterns

- Navigation labels are short nouns: Reference, Commands, CLI, Manual,
  Guides, Concepts (uv, Deno, Cargo, gh, kubectl, git). A command's page
  is titled by the command's name.
- The reference is its own branch, beside guides (all but Terraform,
  which nests command pages under tasks), a page per command (all but uv).
- A command's page: usage, description, options, examples; most end with
  "See also" or "Related documentation", often linking related commands.
- Reference examples are commands only in every tool surveyed; output is
  in guides (Cargo, Deno, gh, kubectl, ripgrep) or a prose section
  (Terraform, gh's "In use").
- Global options documented once and linked (mise, Terraform; gh and
  kubectl in an inherited block); uv and Cargo repeat them on every page.
- A sidebar of every command (gh, mise, Deno, kubectl, Cargo, Terraform);
  Cargo and Deno add prev/next.
- Grouping by job: git, mise, Deno (headings of two to five words);
  Cargo by kind; gh and kubectl flat.
- A cheat sheet beside the generated reference: kubectl, git.
