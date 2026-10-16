#!/usr/bin/env python3
"""PreToolUse on Bash: a git command that creates a branch, pushes to a
branch other than main, or deletes a remote ref needs the user's
confirmation; pushing tags does not. This repository
works on main (AGENTS.md, "Version control"); a feature branch is made
only when the user asks for one, and then they confirm it here."""
import json
import os
import re
import shlex
import subprocess
import sys

event = json.load(sys.stdin)
command = event.get("tool_input", {}).get("command", "")


def git(args, directory):
    try:
        return subprocess.run(["git", *args], capture_output=True, text=True, timeout=5,
                              cwd=directory or event.get("cwd") or None)
    except Exception:
        return None


def current_branch(directory):
    result = git(["branch", "--show-current"], directory)
    return result.stdout.strip() if result else ""


def is_tag(name, directory):
    """A name pushed without refs/: a tag when the repository has a tag of
    that name and no branch of it."""
    def has(ref):
        result = git(["rev-parse", "--verify", "--quiet", ref], directory)
        return bool(result and result.returncode == 0)
    return has("refs/tags/" + name) and not has("refs/heads/" + name)


def reason(words):
    if not words or words[0] != "git":
        return None
    words, directory = words[1:], cd
    while words and words[0] in ("-C", "-c"):
        if words[0] == "-C" and len(words) > 1:
            directory = words[1]
        words = words[2:]
    if not words:
        return None
    sub, rest = words[0], words[1:]
    if sub == "checkout" and any(w in ("-b", "-B") for w in rest):
        return "creates a branch"
    if sub == "switch" and any(w in ("-c", "-C", "--create", "--force-create") for w in rest):
        return "creates a branch"
    if sub == "branch":
        names = [w for w in rest if not w.startswith("-")]
        listing = any(w in ("-d", "-D", "--delete", "-l", "--list", "-a", "-r", "--show-current",
                            "-u", "--set-upstream-to", "--unset-upstream", "--contains", "--merged")
                      for w in rest)
        if names and not listing:
            return "creates a branch"
    if sub == "push":
        if any(w in ("-d", "--delete") for w in rest) or any(w.startswith(":") for w in rest):
            return "deletes a remote ref"
        words = [w for w in rest if not w.startswith("-")][1:]
        refs = []
        while words:
            if words[0] == "tag" and len(words) > 1:  # git push origin tag <name>
                words = words[2:]
                continue
            refs.append(words.pop(0))
        targets = []
        for ref in refs:
            source, _, target = ref.lstrip("+").rpartition(":")
            target = target or source
            if target.startswith("refs/tags/"):
                continue
            if not target.startswith("refs/heads/") and is_tag(target, directory):
                continue
            target = target.removeprefix("refs/heads/")
            targets.append(current_branch(directory) if target == "HEAD" else target)
        if not refs and "--tags" not in rest:
            targets = [current_branch(directory)]
        if any(t != "main" for t in targets):
            return "pushes to " + ", ".join(t or "a detached HEAD" for t in targets)
    return None


cd = None  # the directory a `cd` earlier in the command moved to
for part in re.split(r"&&|\|\||;|\|", command):
    try:
        words = shlex.split(part)
    except ValueError:
        continue
    if len(words) == 2 and words[0] == "cd":
        cd = os.path.join(cd or event.get("cwd") or os.getcwd(), os.path.expanduser(words[1]))
        continue
    while words and "=" in words[0] and not words[0].startswith("-"):
        words = words[1:]
    found = reason(words)
    if found:
        print(json.dumps({"hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "ask",
            "permissionDecisionReason": "This repository works on main; this command " + found +
            ". Confirm only if you asked for it.",
        }}))
        break
