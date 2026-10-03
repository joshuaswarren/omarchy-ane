#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Drive the promotion flow from a promotion_check verdict: propose PRs, gate
and merge them, cut the release. The workflow (.github/workflows/promotion.yml)
calls this; every step is also runnable by hand.

  promote_from_verdict.py propose  --verdict-file verdict.json [--dry-run]
  promote_from_verdict.py gate     --verdict-file verdict.json [--dry-run]
  promote_from_verdict.py release  --chip t8112 --merge-sha <sha> [--verdict-file F] [--dry-run]
  promote_from_verdict.py aurora-plan --tree LINUX --out PLAN [--verdict-file F]
  promote_from_verdict.py aurora-pr   --plan PLAN [--verdict-file F] [--dry-run]

  --verdict-file -     verdict JSON from `promotion_check.py --json` ("-" is
                       stdin). Without it the tool runs promotion_check.py
                       --remote --json for a fresh live verdict.
  --dry-run            print every action instead of doing it; exit 0.

propose: for every PROMOTE chip (state opt-in) it commits the promote_chip
flip on auto/promote-<chip> and opens or updates one PR labeled auto-promotion
whose body hides the exact verdict block in an HTML comment; gate compares
that block against a fresh verdict and refuses when new evidence landed. Every
REVERT chip (state on) gets the reverse PR, additionally labeled urgent. A
second run updates the same PR and rewrites its block, never opens a second
one. An open auto/promote-* or auto/revert-* PR whose chip this run does not
propose (no longer PROMOTE or REVERT, or already flipped on main) is closed
with a comment, so no stale PR is left for the gate.

gate: (the workflow runs the host tests on the PR head first, then returns to
the base branch) each open auto-promotion PR's recorded chip verdict is
checked against a fresh verdict computed on the base branch: on the PR head
the chip is already flipped, so its verdict there is ON or STAY, never the
PROMOTE or REVERT the PR was proposed from. gate refuses to run on a checkout
that already has the PR's flip. The fresh verdict still backs the PR when the
verdict and the targets are the same, every judged row the PR was proposed
from is still judged with the same outcome (passed, driver_source), and no new
judged row fails. A new judged passing row adds support (one passing row
promotes), and rows that are not judged never count. Then it squash-merges via
REST; otherwise the run fails and the next proposal supersedes the PR.

release: compute the next patch version from the latest v* tag, move the
CHANGELOG Unreleased section under "## X.Y.Z (UTC date)", push that to main,
tag the merge commit, publish the GitHub release naming the chip and the row
shas, and print the source-tarball sha256 for the job summary.

aurora-plan and aurora-pr: for every chip whose targets include aurora-dt (a
PROMOTE, or an ON chip, with a passing row that ran the kernel's own driver,
driver_source=intree), the device-tree change in aurora-silicon/linux. The
work is split so that the token never shares a process, or a runner, with code
from that tree.

aurora-plan builds LINUX, a checkout of AURORA_BASE (tools/asahi-dtbs
OUT/linux; its dtc on PATH), with tools/aurora_dt.py, and writes PLAN: the
checkout's HEAD and, per chip, the file, the change (how and refs), the boards
and nodes it checked, and the sha256 of the new file. It calls no GitHub API
and needs no token. A chip whose ANE LINUX already enables gets no entry. A
chip with no apple,*-ane node on any board (LINUX predates the in-tree driver)
gets no entry and a line that starts with SKIPPED; the run still exits 0. Any
other refusal from aurora_dt.py fails the run.

aurora-pr builds and runs nothing from LINUX. It checks that AURORA_BASE's tip
is the PLAN's HEAD, reads the chip's file at that commit from the GitHub API,
makes the change again with aurora_dt.render(), and refuses before any write
unless the result has the PLAN's sha256. Then it commits through the Git Data
API on AURORA_FORK, branch AURORA_BRANCH, opens or updates one PR to AURORA
(head AURORA_FORK:branch, base AURORA_BASE), and comments on
AURORA#AURORA_TRACKING for the in-tree tester unless a comment there already
carries this chip's and PR's marker. GH_TOKEN must then be a token that can
push to AURORA_FORK and open PRs and comments on AURORA, nothing more (README
"Promotion"). With --dry-run it prints the diff, branch, title, body and
comment and calls nothing. A tip that moved fails the run; the next run plans
again.

A synthetic verdict file is refused when GITHUB_REPOSITORY is
joshuaswarren/omarchy-ane (the production repo only ever judges live rows).
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import textwrap
import urllib.request
from pathlib import Path

import aurora_dt
from promotion_check import AURORA_DT, DRIVER_SOURCE, INTREE, ON

REPO = Path(__file__).resolve().parents[1]
PRODUCTION = "joshuaswarren/omarchy-ane"
MARKETING = aurora_dt.MARKETING
LABEL = "auto-promotion"
BRANCH = {"default-on": "auto/promote-{chip}", "opt-in": "auto/revert-{chip}"}
MARK = "<!-- promotion-verdict\n"
DRY = False
# The aurora-dt target: the in-tree kernel's own device tree.
AURORA = "aurora-silicon/linux"
AURORA_BASE = "aurora-wip"
AURORA_FORK = "joshuaswarren/aurorasilicon-linux"
AURORA_BRANCH = "omarchy-ane/enable-{chip}-ane"
# The in-tree driver PR, and the tester who runs aurora kernels there.
AURORA_TRACKING, AURORA_TESTER = 155, "iconidentify"
AUTHOR = {"name": "Joshua Warren", "email": "816217+joshuaswarren@users.noreply.github.com"}


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


def judged_rows(c):
    return {r["row_sha"]: (r["passed"], r.get(DRIVER_SOURCE)) for r in c["rows"] if r["judged"]}


def new_evidence(recorded, fresh):
    """Why FRESH no longer backs the decision RECORDED was proposed from; empty
    when it still does. A new judged passing row only adds support."""
    out = [f"{key} {recorded.get(key, [])} -> {fresh.get(key, [])}" for key in ("verdict", "targets")
           if recorded.get(key, []) != fresh.get(key, [])]
    old, new = judged_rows(recorded), judged_rows(fresh)
    show = lambda o: "gone" if o is None else f"passed={str(o[0]).lower()} driver_source={o[1]}"
    out += [f"judged row {sha} {show(old[sha])} -> {show(new.get(sha))}" for sha in sorted(old) if new.get(sha) != old[sha]]
    out += [f"new judged failing row {sha}" for sha in sorted(new) if sha not in old and not new[sha][0]]
    return out


def read_block(body):
    if MARK not in body:
        raise Fail("the PR body has no promotion-verdict block")
    return json.loads(body.split(MARK, 1)[1].split("\n-->", 1)[0])


def commit_note(c):
    return "row " + (",".join(r["row_sha"] for r in c["rows"] if r["judged"]) or "none")


def propose(verdict):
    slug = repo_slug()
    proposed = set()
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
        # a fresh runner has no remote-tracking ref for the auto branch; without
        # it, --force-with-lease refuses to update an existing branch
        sh("git", "fetch", "origin", f"refs/heads/{branch}:refs/remotes/origin/{branch}",
           check=False)
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
        found = [p for p in gh(f"repos/{slug}/pulls?state=open&per_page=100")
                 if p["head"]["ref"] == branch]
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
        proposed.add(branch)
        sh("git", "checkout", "main")
    if not [c for c in verdict["chips"] if c["verdict"] in ("PROMOTE", "REVERT")]:
        print("propose: nothing to do (no PROMOTE or REVERT chip)")
    if DRY:
        return
    flip = re.compile(r"auto/(promote|revert)-(t[0-9]+)")
    for p in gh(f"repos/{slug}/pulls?state=open&per_page=100"):
        m = flip.fullmatch(p["head"]["ref"])
        if not m or p["head"]["ref"] in proposed:
            continue
        chip = m.group(2)
        now = next((c["verdict"] for c in verdict["chips"] if c["chip"] == chip), "no rows")
        gh(f"repos/{slug}/issues/{p['number']}/comments", "POST",
           {"body": f"Superseded: the fresh verdict for {chip} is {now}, so this run proposes no "
                    f"{m.group(1)} for it. Closing; the next PROMOTE or REVERT opens a new PR."})
        gh(f"repos/{slug}/pulls/{p['number']}", "PATCH", {"state": "closed"})
        print(f"propose: closed stale PR #{p['number']} ({chip} is {now})")


def gate(verdict, pr):
    slug = repo_slug()
    body = gh(f"repos/{slug}/pulls/{pr}")["body"]
    recorded = read_block(body)
    chip = recorded["chip"]
    if (recorded["verdict"] == "PROMOTE") == (chip in ON):
        raise Fail(f"PR #{pr}: packaging/dt/overlays in this checkout already has {chip} "
                   f"{'enabled' if chip in ON else 'opt-in'}, the PR's flip, so a fresh verdict here judges the "
                   "flipped tree; run the gate from the base branch, where the flip is not merged")
    fresh = chip_verdict(verdict, chip)
    print(f"gate: PR #{pr} chip {chip}: recorded={recorded['verdict']} fresh={fresh['verdict']}")
    why = new_evidence(recorded, fresh)
    if why:
        raise Fail(f"PR #{pr}: the fresh verdict no longer backs it ({'; '.join(why)}); supersede with a new proposal")
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


def aurora_texts(c, p, base, pr_url="the new PR"):
    """(title, commit message, PR body, tracking comment) for one chip's aurora-dt change."""
    chip, name = c["chip"], MARKETING.get(c["chip"], c["chip"])
    rows = ", ".join(r["row_sha"] for r in c["rows"] if r.get(DRIVER_SOURCE) == INTREE and r["passed"])
    what = (f"Drop APPLE_ANE_UNTESTED from {p['file']}, so its #ifndef block enables"
            if p["how"] == "switch" else f"Set status \"okay\" in {p['file']} on")
    why = textwrap.fill(f"The ANE has run on the {name} ({chip.upper()}) with the in-tree driver: in-tree row(s) "
                        f"{rows} passed the omarchy-ane promotion rule (omarchy-ane-check ready, the chip's driver "
                        "loaded, 20 of 20 smoke calls bit-exact against the golden, no ANE, ANE-DART or "
                        "ANE-mailbox fault).", 72, break_on_hyphens=False)
    change = textwrap.fill(f"{what} {', '.join(p['enabled'])}.", 72, break_on_hyphens=False)
    title = f"arm64: dts: apple: {chip}: Enable the ANE"
    message = f"{title}\n\n{why}\n\n{change}\n\nSigned-off-by: {AUTHOR['name']} <{AUTHOR['email']}>\n"
    side = (f"omarchy-ane already enables the {chip.upper()} overlay by default" if c["state"] == "on" else
            f"The omarchy-ane side of the same verdict is its auto/promote-{chip} PR")
    body = (f"{why}\n\n{change}\n\nChecked by building {', '.join(p['boards'])} from {AURORA_BASE} {base[:12]} "
            "with this tree's dtc, before and after: after the change, every node the ANE reaches is enabled.\n\n"
            f"Generated by joshuaswarren/omarchy-ane tools/promote_from_verdict.py aurora-plan and aurora-pr "
            f"(tools/aurora_dt.py). {side}.\n\n{body_block(c)}")
    comment = (f"@{AURORA_TESTER} In-tree row(s) {rows} on the {name} ({chip.upper()}) passed the omarchy-ane "
               f"promotion rule, and {pr_url} enables the {chip.upper()} ANE in `{p['file']}`. On a kernel with "
               "that change the ANE node comes from the kernel's own device tree; overlay opt-in has no effect there.")
    return title, message, body, comment


def aurora_chips(verdict):
    return [c for c in verdict["chips"] if AURORA_DT in c.get("targets", [])]


def aurora_plan(verdict, tree, out):
    """Build TREE and write OUT, one line of JSON. No GitHub API call, so no token."""
    plan = {"aurora": AURORA, "base_ref": AURORA_BASE, "base": None, "chips": []}
    chips = aurora_chips(verdict)
    if not chips:
        print("aurora-plan: nothing to do (no chip with the aurora-dt target)")
    elif tree is None:
        raise Fail(f"aurora-plan needs --tree, a checkout of {AURORA} {AURORA_BASE}")
    else:
        plan["base"] = subprocess.run(["git", "-C", str(tree), "rev-parse", "HEAD"], capture_output=True, text=True,
                                      check=True).stdout.strip()
    for c in chips:
        chip = c["chip"]
        try:
            p = aurora_dt.plan(tree, chip)
        except aurora_dt.NoNode as e:
            print(f"SKIPPED\t{chip}\t{AURORA} {AURORA_BASE} {plan['base'][:12]}: {e}. The aurora PR can be made only "
                  "after the in-tree driver lands there; the overlay PR is not affected")
            continue
        except aurora_dt.Refuse as e:
            raise Fail(f"aurora-plan: {chip}: {e}")
        if p is None:
            print(f"aurora-plan: {chip}: {AURORA} {AURORA_BASE} {plan['base'][:12]} already enables the ANE; no PR")
            continue
        print(f"aurora-plan: {chip}: {p['how']} in {p['file']} on {plan['base'][:12]}")
        sys.stdout.write(aurora_dt.diff(p))
        plan["chips"].append({"chip": chip, "file": p["file"], "how": p["how"], "refs": p["refs"],
                              "boards": p["boards"], "enabled": p["enabled"],
                              "sha256": hashlib.sha256(p["new"].encode()).hexdigest(), "diff": aurora_dt.diff(p)})
        print(f"PLANNED\t{chip}")
    Path(out).write_text(json.dumps(plan, separators=(",", ":")) + "\n")


def plan_entries(plan, verdict):
    """[(chip verdict, plan entry)] after checking every field the PR uses:
    the plan comes from a job that ran code from the aurora tree."""
    chips = {c["chip"]: c for c in aurora_chips(verdict)}
    if not re.fullmatch(r"[0-9a-f]{40}", str(plan.get("base"))):
        raise Fail("aurora-pr: the plan's base is not a commit sha")
    out = []
    for e in plan["chips"]:
        chip = e.get("chip")
        names = [*e.get("enabled", []), *e.get("boards", [])]
        if chip not in chips:
            raise Fail(f"aurora-pr: {chip}: not a PROMOTE chip with the {AURORA_DT} target in this verdict")
        if (e.get("file") != f"{aurora_dt.DTS}/{chip}.dtsi" or not re.fullmatch(r"[0-9a-f]{64}", str(e.get("sha256")))
                or not all(re.fullmatch(r"[A-Za-z0-9_@,./{}+-]+", str(n)) for n in names)
                or not all(re.fullmatch(rf"{chip}-[a-z0-9]+\.dts", str(b)) for b in e.get("boards", []))):
            raise Fail(f"aurora-pr: {chip}: the plan entry is not one aurora-plan writes")
        out.append((chips[chip], e))
    return out


def tracking_comments():
    found, page = [], 1
    while True:
        batch = gh(f"repos/{AURORA}/issues/{AURORA_TRACKING}/comments?per_page=100&page={page}")
        found += batch
        if len(batch) < 100:
            return found
        page += 1


def aurora_pr(verdict, plan_file):
    """Open or update the AURORA PR from PLAN_FILE. Builds and runs nothing from the tree."""
    plan = json.loads(Path(plan_file).read_text())
    if not plan.get("chips"):
        print("aurora-pr: the plan has no chip; nothing to do")
        return
    base = plan.get("base")
    entries = plan_entries(plan, verdict)
    owner = AURORA_FORK.split("/")[0]
    if DRY:
        for c, e in entries:
            branch = AURORA_BRANCH.format(chip=c["chip"])
            title, message, body, comment = aurora_texts(c, e, base)
            print(f"aurora-pr: {c['chip']}: {e['how']} in {e['file']} on {base[:12]}; branch {AURORA_FORK}:{branch}; "
                  f"PR to {AURORA} {AURORA_BASE}\n--- the plan's diff\n{e['diff']}")
            print(f"aurora-pr: would check that {AURORA_BASE} is at {base[:12]}, read {e['file']} there and make the "
                  f"change again (sha256 {e['sha256'][:12]}), commit through the Git Data API on {AURORA_FORK}, point "
                  f"{branch} at it, open or update one PR, and comment on {AURORA}#{AURORA_TRACKING} unless a comment "
                  f"there has its marker\n--- title\n{title}\n--- commit message\n{message}--- PR body\n{body}\n"
                  f"--- comment\n{comment}")
        return
    tip = gh(f"repos/{AURORA}/git/ref/heads/{AURORA_BASE}")["object"]["sha"]
    if tip != base:
        raise Fail(f"aurora-pr: {AURORA} {AURORA_BASE} is at {tip[:12]}, but the plan was built on {base[:12]}; "
                   "nothing written, the next run plans again")
    base_tree = gh(f"repos/{AURORA}/git/commits/{base}")["tree"]["sha"]
    changes = []
    for c, e in entries:
        old = base64.b64decode(gh(f"repos/{AURORA}/contents/{e['file']}?ref={base}")["content"]).decode()
        try:
            new = aurora_dt.render(old, c["chip"], e["how"], e["refs"])
        except aurora_dt.Refuse as err:
            raise Fail(f"aurora-pr: {c['chip']}: {err}; nothing written")
        if hashlib.sha256(new.encode()).hexdigest() != e["sha256"]:
            raise Fail(f"aurora-pr: {c['chip']}: {e['file']} at {base[:12]} with the plan's change does not have the "
                       "plan's sha256; nothing written")
        changes.append((c, e, new))
    for c, e, new in changes:
        chip, branch = c["chip"], AURORA_BRANCH.format(chip=c["chip"])
        title, message, body, _ = aurora_texts(c, e, base)
        new_tree = gh(f"repos/{AURORA_FORK}/git/trees", "POST", {"base_tree": base_tree, "tree": [
            {"path": e["file"], "mode": "100644", "type": "blob", "content": new}]})["sha"]
        commit = gh(f"repos/{AURORA_FORK}/git/commits", "POST", {"message": message, "tree": new_tree,
                                                                 "parents": [base], "author": AUTHOR})["sha"]
        if gh(f"repos/{AURORA_FORK}/git/matching-refs/heads/{branch}"):
            gh(f"repos/{AURORA_FORK}/git/refs/heads/{branch}", "PATCH", {"sha": commit, "force": True})
        else:
            gh(f"repos/{AURORA_FORK}/git/refs", "POST", {"ref": f"refs/heads/{branch}", "sha": commit})
        found = gh(f"repos/{AURORA}/pulls?state=open&head={owner}:{branch}")
        if found:
            n, url = found[0]["number"], found[0]["html_url"]
            gh(f"repos/{AURORA}/pulls/{n}", "PATCH", {"title": title, "body": body})
            print(f"aurora-pr: updated {AURORA}#{n}")
        else:
            pr = gh(f"repos/{AURORA}/pulls", "POST", {"title": title, "body": body, "head": f"{owner}:{branch}",
                                                      "base": AURORA_BASE, "maintainer_can_modify": True})
            n, url = pr["number"], pr["html_url"]
            print(f"aurora-pr: opened {AURORA}#{n}")
        marker = f"<!-- omarchy-ane aurora-dt {chip} {AURORA}#{n} -->"
        if any(marker in (m.get("body") or "") for m in tracking_comments()):
            print(f"aurora-pr: {AURORA}#{AURORA_TRACKING} already has the comment for #{n}")
        else:
            gh(f"repos/{AURORA}/issues/{AURORA_TRACKING}/comments", "POST",
               {"body": f"{aurora_texts(c, e, base, url)[3]}\n\n{marker}"})
            print(f"aurora-pr: commented on {AURORA}#{AURORA_TRACKING}")
        print(f"AURORA\t{n}\t{chip}\t{commit}")


def main(argv=None):
    global DRY
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mode", choices=("propose", "gate", "release", "aurora-plan", "aurora-pr"))
    ap.add_argument("--verdict-file", help="verdict JSON file ('-' is stdin); default: fresh --remote verdict")
    ap.add_argument("--pr", type=int, help="gate: the auto-promotion PR number to gate and merge")
    ap.add_argument("--chip", help="release: the chip that merged")
    ap.add_argument("--merge-sha", help="release: the squash-merge commit to tag")
    ap.add_argument("--tree", type=Path, help=f"aurora-plan: a checkout of {AURORA} {AURORA_BASE}")
    ap.add_argument("--out", type=Path, help="aurora-plan: the plan file to write")
    ap.add_argument("--plan", type=Path, help="aurora-pr: the plan file aurora-plan wrote")
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
    elif args.mode == "aurora-plan":
        if not args.out:
            ap.error("aurora-plan needs --out")
        aurora_plan(verdict, args.tree, args.out)
    elif args.mode == "aurora-pr":
        if not args.plan:
            ap.error("aurora-pr needs --plan")
        aurora_pr(verdict, args.plan)
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
