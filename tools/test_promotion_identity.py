#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""The promotion workflow's commits must satisfy the COS-290 pre-commit identity guard.

The guard (scripts/hooks/pre-commit) refuses a commit unless the GLOBAL identity exists and the
effective identity equals it. The workflow used to set a repo-local identity, which the guard
reads as "differs from the contributor's global identity" the moment hooks are enabled in CI.
This test pins the fixed shape in two layers:

  1. source: promotion.yml sets the automation identity with `git config --global`, never repo-local;
  2. behavior: in a throwaway repo with the real hook installed and a fake HOME, the workflow's
     exact commands make a commit pass, while the old repo-local form is refused with the guard's
     own message.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORKFLOW = REPO / ".github/workflows/promotion.yml"
HOOK = REPO / "scripts/hooks/pre-commit"

failures = []


def check(ok, label, detail=""):
    print(("PASS " if ok else "FAIL ") + label + (f": {detail}" if detail and not ok else ""))
    if not ok:
        failures.append(label)


def run(cmd, cwd, env):
    return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, check=False)


text = WORKFLOW.read_text()

# 1. Source layer: no repo-local identity form anywhere in the workflow.
repo_local = re.findall(r"^\s*git config (?!.*--global\b).*user\.(?:name|email) .*$", text, re.MULTILINE)
check(not repo_local, "promotion.yml has no repo-local git config identity",
      f"found {len(repo_local)}: {repo_local[:3]}")

global_lines = re.findall(r"^\s*git config --global user\.name (\S+)\s*$", text, re.MULTILINE)
global_emails = re.findall(r"^\s*git config --global user\.email (\S+)\s*$", text, re.MULTILINE)
check(len(global_lines) >= 3 and len(global_emails) >= 3,
      "promotion.yml sets a global automation identity in every committing job",
      f"{len(global_lines)} name lines, {len(global_emails)} email lines")
# Canonical automation identity: tools/promote_from_verdict.py commits with these exact
# values via -c overrides, so the workflow-set global must match them or the guard
# would refuse the real automation commits (identity drift is what this pin catches).
auto_name = "omarchy-ane-automation"
auto_email = "816217+joshuaswarren@users.noreply.github.com"
check(all(v == auto_name for v in global_lines) and all(v == auto_email for v in global_emails),
      "promotion.yml global identity equals the identity the tool commits with",
      f"names={sorted(set(global_lines))} emails={sorted(set(global_emails))}")

# 2. Behavior layer: the real hook in a throwaway repo with a fake HOME.
with tempfile.TemporaryDirectory(prefix="w7j-promo-id-") as tmp:
    home = Path(tmp) / "home"
    home.mkdir()
    repo = Path(tmp) / "repo"
    repo.mkdir()
    # Drop ambient git identity/config env so a local shell cannot inject values
    # that outrank the fake global and flip the behavioral checks.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("GIT_CONFIG_COUNT", "GIT_CONFIG_KEY_", "GIT_CONFIG_VALUE_"))
           and k not in ("GIT_CONFIG_PARAMETERS", "GIT_AUTHOR_NAME", "GIT_AUTHOR_EMAIL",
                         "GIT_COMMITTER_NAME", "GIT_COMMITTER_EMAIL")}
    env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / ".config"),
               GIT_CONFIG_GLOBAL=str(home / ".gitconfig"))
    run(["git", "init", "-q", "-b", "main", str(repo)], tmp, env)
    hooks = repo / "scripts" / "hooks"
    hooks.mkdir(parents=True)
    (hooks / "pre-commit").write_text(HOOK.read_text())
    (hooks / "pre-commit").chmod(0o755)
    run(["git", "config", "core.hooksPath", "scripts/hooks"], str(repo), env)

    commit = ["git", "-c", f"user.name={auto_name}", "-c", f"user.email={auto_email}",
              "commit", "--allow-empty", "-m", "automation commit"]

    # The bug shape: repo-local identity only (the old workflow form). The guard must refuse.
    run(["git", "config", "user.name", auto_name], str(repo), env)
    run(["git", "config", "user.email", auto_email], str(repo), env)
    refused = run(commit, str(repo), env)
    check(refused.returncode != 0 and "differs from the contributor's global identity" in refused.stderr,
          "repo-local-only identity is refused by the guard",
          f"rc={refused.returncode} stderr={refused.stderr[:200]}")

    # The fix shape: the workflow's exact global commands, then the tool's commit form. Must pass.
    run(["git", "config", "--global", "user.name", auto_name], str(repo), env)
    run(["git", "config", "--global", "user.email", auto_email], str(repo), env)
    ok = run(commit, str(repo), env)
    check(ok.returncode == 0, "workflow global identity + tool commit form passes the guard",
          f"rc={ok.returncode} stderr={ok.stderr[:200]}")
    who = run(["git", "log", "-1", "--format=%aN|%cN"], str(repo), env)
    check(who.stdout.strip() == f"{auto_name}|{auto_name}",
          "author and committer are the automation identity", who.stdout.strip())

if failures:
    print(f"FAIL promotion identity test: {len(failures)} check(s) failed")
    sys.exit(1)
print("PASS promotion identity test")
