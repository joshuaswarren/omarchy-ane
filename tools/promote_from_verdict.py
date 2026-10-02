#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Drive the promotion flow from a promotion_check verdict: propose PRs, gate
and merge them, cut the release. The workflow (.github/workflows/promotion.yml)
calls this; every step is also runnable by hand.

  promote_from_verdict.py propose  --verdict-file verdict.json [--dry-run]
  promote_from_verdict.py gate     --verdict-file verdict.json [--dry-run]
  promote_from_verdict.py release  --chip t8112 --merge-sha <sha> [--verdict-file F] [--dry-run]

  --verdict-file -     verdict JSON from `promotion_check.py --json` ("-" is
                       stdin). Without it the tool runs promotion_check.py
                       --remote --json for a fresh live verdict.
  --dry-run            print every action instead of doing it; exit 0.

propose: for every PROMOTE chip (state opt-in) it commits the promote_chip
flip on auto/promote-<chip> and opens or updates one PR labeled auto-promotion
whose body hides the exact verdict block in an HTML comment; gate compares
that block against a fresh verdict and refuses when new evidence landed. Every
REVERT chip (state on) gets the reverse PR, additionally labeled urgent. A
second run updates the same PR, never opens a second one.

gate: (the workflow runs the host tests first) each open auto-promotion PR's
recorded chip verdict must equal the fresh verdict; equal means squash-merge
via REST. Unequal (a new row landed) fails the run; the next proposal
supersedes the PR.

release: compute the next patch version from the latest v* tag, move the
CHANGELOG Unreleased section under "## X.Y.Z (UTC date)", push that to main,
tag the merge commit, publish the GitHub release naming the chip and the row
shas, and print the source-tarball sha256 for the job summary.

A synthetic verdict file is refused when GITHUB_REPOSITORY is
joshuaswarren/omarchy-ane (the production repo only ever judges live rows).
"""
import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PRODUCTION = "joshuaswarren/omarchy-ane"
MARKETING = {"t8103": "M1", "t6000": "M1 Pro", "t6001": "M1 Max", "t6002": "M1 Ultra",
             "t8112": "M2", "t6020": "M2 Pro", "t6021": "M2 Max", "t6022": "M2 Ultra"}
LABEL = "auto-promotion"
BRANCH = {"default-on": "auto/promote-{chip}", "opt-in": "auto/revert-{chip}"}
MARK = "<!-- promotion-verdict\n"
DRY = False


class Fail(Exception):
    pass


def sh(*cmd, check=True):
    print(f"+ {subprocess.list2cmdline(cmd)}")
    if DRY:
        return ""
    p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if check and p.returncode:
        raise Fail(f"{' '.join(cmd)} failed: {p.stderr.strip() or p.stdout.strip()}")
    return p.stdout


def gh(endpoint, method="GET", body=None):
    """One REST call via the gh CLI (GITHUB_TOKEN)."""
    cmd = ["gh", "api", endpoint]
    if method != "GET":
        cmd += ["-X", method]
    if body is not None:
        cmd += ["--input", "-"]
    print(f"+ gh api {method} {endpoint}")
    if DRY:
        return {} if method == "GET" else None
    p = subprocess.run(cmd, cwd=REPO, input=json.dumps(body) if body is not None else None,
                       capture_output=True, text=True)
    if p.returncode:
        raise Fail(f"gh api {method} {endpoint} failed: {p.stderr.strip()}")
    return json.loads(p.stdout) if p.stdout.strip() else {}


def repo_slug():
    r = os.environ.get("GITHUB_REPOSITORY")
    if r:
        return r
    out = subprocess.run(["git", "-C", str(REPO), "remote", "get-url", "origin"],
                         capture_output=True, text=True).stdout.strip()
    if "github.com" not in out:
        return PRODUCTION
    return out.split("github.com")[-1].lstrip(":/").removesuffix(".git")


def load_verdict(path):
    if path is None:
        p = subprocess.run([sys.executable, str(REPO / "tools/promotion_check.py"),
                            "--remote", "--json"], cwd=REPO, capture_output=True, text=True)
        if p.returncode:
            raise Fail(f"promotion_check --remote --json failed: {p.stderr.strip()}")
        return json.loads(p.stdout)
    if os.environ.get("GITHUB_REPOSITORY") == PRODUCTION:
        raise Fail("a synthetic verdict file is refused in " + PRODUCTION)
    text = sys.stdin.read() if path == "-" else Path(path).read_text()
    return json.loads(text)


def chip_verdict(verdict, chip):
    for c in verdict["chips"]:
        if c["chip"] == chip:
            return c
    raise Fail(f"verdict has no chip {chip}")


def chip_text(c):
    rows = "; ".join(f"{r['row_sha']} judged={str(r['judged']).lower()} passed={str(r['passed']).lower()}"
                     + (f" reasons={'; '.join(r['reasons'])}" if r["reasons"] else "")
                     for r in c["rows"]) or "no rows"
    return f"{c['chip']}: state={c['state']} verdict={c['verdict']}\n  rows: {rows}"


def body_block(c):
    return MARK + json.dumps(c, sort_keys=True) + "\n-->"


def read_block(body):
    if MARK not in body:
        raise Fail("the PR body has no promotion-verdict block")
    return json.loads(body.split(MARK, 1)[1].split("\n-->", 1)[0])


def commit_note(c):
    return "row " + (",".join(r["row_sha"] for r in c["rows"] if r["judged"]) or "none")


def propose(verdict):
    slug = repo_slug()
    for c in verdict["chips"]:
        if c["verdict"] not in ("PROMOTE", "REVERT"):
            continue
        to = "default-on" if c["verdict"] == "PROMOTE" else "opt-in"
        branch = BRANCH[to].format(chip=c["chip"])
        print(f"propose: {c['chip']} -> {to} on {branch}")
        if DRY:
            print(f"propose: would commit the flip, push {branch}, open or update one PR labeled "
                  f"{LABEL}" + (" and urgent" if c["verdict"] == "REVERT" else ""))
            print("propose: the gate would then run the host tests on that PR, compare the fresh "
                  "verdict, and squash-merge it; the release would tag the next patch version, "
                  "move the CHANGELOG Unreleased section and publish")
            continue
        sh("git", "fetch", "origin", "main")
        sh("git", "checkout", "-B", branch, "origin/main")
        sh(sys.executable, str(REPO / "tools/promote_chip.py"), "--chip", c["chip"],
           "--to", to, "--apply", "--note", commit_note(c))
        changed = sh("git", "status", "--porcelain").strip()
        if not changed:
            print(f"propose: {c['chip']} is already {to} on main; nothing to propose")
            sh("git", "checkout", "main")
            continue
        sh("git", "add", "-A")
        sh("git", "-c", "user.name=omarchy-ane-automation",
           "-c", "user.email=816217+joshuaswarren@users.noreply.github.com",
           "commit", "-m", f"{c['verdict']}: {c['chip']} ANE -> {to} ({commit_note(c)})")
        sh("git", "push", "--force-with-lease", "origin", branch)
        title = f"{c['verdict']}: {c['chip']} ({MARKETING.get(c['chip'], c['chip'])}) ANE -> {to}"
        body = ("Automatic promotion-flow PR from the promotion_check verdict.\n\n"
                f"```\n{chip_text(c)}\n```\n\n{body_block(c)}")
        found = gh(f"repos/{slug}/pulls?state=open&head={slug.replace('/', '%2F')}:{branch}")
        if found:
            n = found[0]["number"]
            gh(f"repos/{slug}/pulls/{n}", "PATCH", {"title": title, "body": body})
            print(f"propose: updated PR #{n}")
        else:
            pr = gh(f"repos/{slug}/pulls", "POST",
                    {"title": title, "body": body, "head": branch, "base": "main"})
            n = pr["number"]
            gh(f"repos/{slug}/issues/{n}/labels", "POST",
               {"labels": [LABEL] + (["urgent"] if c["verdict"] == "REVERT" else [])})
            print(f"propose: opened PR #{n}")
        print(f"PROPOSED\t{n}\t{c['chip']}")
        sh("git", "checkout", "main")
    if not [c for c in verdict["chips"] if c["verdict"] in ("PROMOTE", "REVERT")]:
        print("propose: nothing to do (no PROMOTE or REVERT chip)")


def gate(verdict, pr):
    slug = repo_slug()
    body = gh(f"repos/{slug}/pulls/{pr}")["body"]
    recorded = read_block(body)
    fresh = chip_verdict(verdict, recorded["chip"])
    print(f"gate: PR #{pr} chip {recorded['chip']}: "
          f"recorded={recorded['verdict']} fresh={fresh['verdict']}")
    if recorded != fresh:
        raise Fail(f"PR #{pr}: the fresh verdict differs from the one the PR was proposed from "
                   f"(new evidence landed); supersede with a new proposal")
    merge = gh(f"repos/{slug}/pulls/{pr}/merge", "PUT",
               {"merge_method": "squash",
                "commit_title": f"{recorded['verdict']}: {recorded['chip']} ANE ({commit_note(recorded)})"})
    if not merge.get("merged"):
        raise Fail(f"PR #{pr}: merge refused: {merge.get('message')}")
    print(f"gate: merged PR #{pr} as {merge['sha']}")
    print(f"MERGED\t{recorded['chip']}\t{merge['sha']}")


def next_version():
    sh("git", "fetch", "--tags", "origin")
    tags = [t for t in sh("git", "tag", "--list", "v*").split() if t]
    latest = max(tags, key=lambda t: [int(x) for x in t[1:].split(".")]) if tags else "v0.0.0"
    x, y, z = (int(v) for v in latest[1:].split("."))
    return f"v{x}.{y}.{z + 1}"


def release(verdict, chip, merge_sha):
    slug = repo_slug()
    c = chip_verdict(verdict, chip)
    version = next_version() if not DRY else "vX.Y.Z"
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    heading = f"## {version[1:]} ({date})"
    changelog = (REPO / "CHANGELOG.md").read_text()
    if "## Unreleased" not in changelog:
        raise Fail("CHANGELOG.md has no Unreleased section")
    head, rest = changelog.split("## Unreleased", 1)
    section, tail = rest.split("\n## ", 1) if "\n## " in rest else (rest, "")
    moved = f"{head}{heading}{section}\n\n## Unreleased\n\n## {tail}" if tail \
        else f"{head}{heading}{section}\n\n## Unreleased\n"
    print(f"release: CHANGELOG Unreleased moves under {heading}")
    if not DRY:
        (REPO / "CHANGELOG.md").write_text(moved)
        sh("git", "add", "CHANGELOG.md")
        sh("git", "-c", "user.name=omarchy-ane-automation",
           "-c", "user.email=816217+joshuaswarren@users.noreply.github.com",
           "commit", "-m", f"Release {version[1:]}: {chip} ANE")
        sh("git", "push", "origin", "HEAD:main")
        sh("git", "tag", version, merge_sha)
        sh("git", "push", "origin", version)
        notes = (f"{chip} ({MARKETING.get(chip, chip)}) ANE release {version[1:]}.\n\n"
                 f"Verdict: {c['verdict']}. Judged rows: {commit_note(c)}\n"
                 f"Checker artifact: https://github.com/{slug}/actions/runs/"
                 f"{os.environ.get('GITHUB_RUN_ID', '')}")
        gh(f"repos/{slug}/releases", "POST", {"tag_name": version, "name": version, "body": notes})
        tarball = urllib.request.urlopen(f"https://github.com/{slug}/archive/refs/tags/{version}.tar.gz",
                                         timeout=60).read()
        digest = hashlib.sha256(tarball).hexdigest()
        print(f"release: published {version} for {chip}; tarball sha256 {digest}")
        summary = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary:
            with open(summary, "a") as f:
                f.write(f"`{version}` {chip}: tarball sha256 `{digest}`\n")


def main(argv=None):
    global DRY
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=("propose", "gate", "release"))
    ap.add_argument("--verdict-file", help="verdict JSON file ('-' is stdin); default: fresh --remote verdict")
    ap.add_argument("--pr", type=int, help="gate: the auto-promotion PR number to gate and merge")
    ap.add_argument("--chip", help="release: the chip that merged")
    ap.add_argument("--merge-sha", help="release: the squash-merge commit to tag")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    DRY = args.dry_run
    verdict = load_verdict(args.verdict_file)
    if args.mode == "propose":
        propose(verdict)
    elif args.mode == "gate":
        if not args.pr:
            ap.error("gate needs --pr")
        gate(verdict, args.pr)
    else:
        if not args.chip or not args.merge_sha:
            ap.error("release needs --chip and --merge-sha")
        release(verdict, args.chip, args.merge_sha)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print(f"promote_from_verdict: {e}", file=sys.stderr)
        sys.exit(1)
