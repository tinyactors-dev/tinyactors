#!/usr/bin/env python3
"""PreToolUse on Bash: a git command that creates a branch, or pushes to a
branch other than main, needs the user's confirmation. This repository
works on main (AGENTS.md, "Version control"); a feature branch is made
only when the user asks for one, and then they confirm it here."""
import json
import re
import shlex
import subprocess
import sys

event = json.load(sys.stdin)
command = event.get("tool_input", {}).get("command", "")


def current_branch():
    try:
        return subprocess.run(["git", "branch", "--show-current"], capture_output=True, text=True,
                              cwd=event.get("cwd") or None, timeout=5).stdout.strip()
    except Exception:
        return ""


def reason(words):
    if len(words) < 2 or words[0] != "git":
        return None
    sub, rest = words[1], words[2:]
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
        refs = [w for w in rest if not w.startswith("-")][1:]
        targets = [r.split(":")[-1].removeprefix("refs/heads/") for r in refs]
        targets = [t for t in targets if t and not t.startswith("refs/tags/")]
        if "--tags" in rest and not targets:
            return None
        targets = [current_branch() if t == "HEAD" else t for t in targets or ["HEAD"]]
        if any(t != "main" for t in targets):
            return "pushes to " + ", ".join(t or "a detached HEAD" for t in targets)
    return None


for part in re.split(r"&&|\|\||;|\|", command):
    try:
        words = shlex.split(part)
    except ValueError:
        continue
    while words and "=" in words[0] and not words[0].startswith("-"):
        words = words[1:]
    found = reason(words)
    if found:
        print(json.dumps({"hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": "ask",
            "permissionDecisionReason": "This repository works on main; this command " + found +
            ". Confirm only if you asked for a feature branch.",
        }}))
        break
