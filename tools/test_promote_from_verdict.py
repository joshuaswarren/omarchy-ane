#!/usr/bin/env python3
"""Offline checks for tools/promote_from_verdict.py: the dry-run plans, the
production refusal of a synthetic verdict file, a real propose double-run
against a stub gh (second run updates the PR, never duplicates it), the gate
compare-and-merge on the recorded verdict block, its refusal on a changed
verdict, the release plan, and the aurora-plan/aurora-pr split: the plan calls
no API, the PR step builds nothing, checks the base tip and re-derives the
change, and comments on the tracking PR once. No network; gh is a stub."""
import base64
import copy
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

repo = Path(__file__).resolve().parents[1]
STUB = Path(tempfile.mkdtemp()) / "gh"
STUB_STATE = Path(tempfile.mkdtemp()) / "state.json"
STUB_LOG = Path(tempfile.mkdtemp()) / "log.jsonl"
FIXTURES = Path(tempfile.mkdtemp())


def verdict(chips):
    p = FIXTURES / f"v{len(list(FIXTURES.iterdir()))}.json"
    p.write_text(json.dumps({"chips": chips}))
    return str(p)


def chip(c, v, state="opt-in", sha="deadbeef1234", passed=True, reasons=None):
    return {"chip": c, "state": state, "verdict": v,
            "rows": [{"row_sha": sha, "judged": True, "passed": passed, "reasons": reasons or []}]}


PROMOTE = verdict([chip("t8112", "PROMOTE")])
REVERT = verdict([chip("t6021", "REVERT", state="on", passed=False, reasons=["check not ready"])])
CONFLICT = verdict([chip("t6000", "CONFLICT", passed=False, reasons=["smoke 19 calls, 19 not bit-exact"])])
STAY = verdict([{"chip": "t6020", "state": "opt-in", "verdict": "STAY", "rows": []}])
ON = verdict([{"chip": "t6001", "state": "on", "verdict": "ON", "rows": []}])


def run(tool_args, cwd, env=None, stub=True):
    e = {**os.environ, "PATH": f"{STUB.parent}:{os.environ['PATH']}",
         "GITHUB_REPOSITORY": "t/repo", "GH_TOKEN": "stub", "STUB_STATE": str(STUB_STATE), "STUB_LOG": str(STUB_LOG)}
    if env:
        e.update(env)
    return subprocess.run([sys.executable, "tools/promote_from_verdict.py", *tool_args],
                          capture_output=True, text=True, env=e, cwd=cwd)


STUB.write_text(f"""#!/usr/bin/env python3
import base64, json, os, sys
a = sys.argv[1:]
ep = a[1] if a[0] == "api" else a[0]
method = a[a.index("-X") + 1] if "-X" in a else "GET"
body = json.loads(sys.stdin.read()) if "--input" in a else None
s = json.load(open(os.environ["STUB_STATE"]))
open(os.environ["STUB_LOG"], "a").write(json.dumps([ep, method, body]) + "\\n")
def out(o): print(json.dumps(o))
if ep.startswith("repos/aurora-silicon/linux/") or "/git/" in ep:
    if "/git/ref/heads/" in ep: out({{"object": {{"sha": s["tip"]}}}})
    elif "/git/commits/" in ep: out({{"tree": {{"sha": "stubbasetree"}}}})
    elif "/contents/" in ep: out({{"content": base64.encodebytes(s["file"].encode()).decode()}})
    elif "/git/trees" in ep: out({{"sha": "stubtree"}})
    elif "/git/commits" in ep: out({{"sha": "stubcommit"}})
    elif "/git/matching-refs/" in ep: out(s.get("refs", []))
    elif ep.endswith("/git/refs"): s["refs"] = [{{"ref": body["ref"]}}]; out({{}})
    elif "/comments" in ep and method == "POST":
        if s.pop("fail_comment", False):
            json.dump(s, open(os.environ["STUB_STATE"], "w")); sys.exit("stub: comment refused")
        s["comments"] = s.get("comments", []) + [{{"body": body["body"]}}]; out({{}})
    elif "/comments" in ep: out(s.get("comments", []) if "page=1" in ep else [])
    elif "pulls?state=open" in ep: out(s.get("aurora", []))
    elif ep.endswith("/pulls"):
        s["aurora"] = [{{"number": 900, "html_url": "https://github.com/aurora-silicon/linux/pull/900"}}]
        out(s["aurora"][0])
    else: out({{}})
elif "pulls?state=open" in ep:
    out(s["open"])
elif ep.endswith("/pulls") and method == "POST":
    n = s["next"]; s["next"] += 1
    s["open"] = s["open"] + [{{"number": n, "labels": [], "head": {{"ref": body["head"], "repo": {{"full_name": "t/repo"}}}}}}]
    s["bodies"][str(n)] = body["body"]
    out({{"number": n, "body": body["body"]}})
elif "/pulls/" in ep and method == "PATCH":
    n = ep.rsplit("/", 1)[1]
    if body.get("state") == "closed":
        s["open"] = [p for p in s["open"] if p["number"] != int(n)]
    else:
        s["bodies"][n] = body["body"]
    out({{"number": int(n)}})
elif "/labels" in ep:
    n = ep.split("/issues/")[1].split("/")[0]
    for p in s["open"]:
        if p["number"] == int(n): p["labels"] = [{{"name": l}} for l in body["labels"]]
    out({{}})
elif "/pulls/" in ep and ep.split("/")[-1].isdigit() and method == "GET":
    n = ep.split("/")[-1]
    labels = next((p["labels"] for p in s["open"] if p["number"] == int(n)), [])
    out({{"number": int(n), "body": s["bodies"][n], "labels": labels}})
elif ep.endswith("/merge"):
    out({{"merged": True, "sha": "stub0merge1sha"}})
json.dump(s, open(os.environ["STUB_STATE"], "w"))
""")
STUB.chmod(0o755)


def make_work():
    """A git clone of a working-tree copy, with origin and main checked out."""
    tmp = Path(tempfile.mkdtemp())
    bare, work = tmp / "origin.git", tmp / "work"
    shutil.copytree(repo, work, ignore=shutil.ignore_patterns(".git"))
    subprocess.run(["git", "init", "-q", "--bare", "-b", "main", str(bare)], check=True)
    subprocess.run(["git", "init", "-q", "-b", "main", str(work)], check=True)
    subprocess.run(["git", "add", "-A"], cwd=work, check=True)
    subprocess.run(["git", "-c", "user.name=Joshua Warren",
                    "-c", "user.email=816217+joshuaswarren@users.noreply.github.com",
                    "commit", "-qm", "base"], cwd=work, check=True)
    subprocess.run(["git", "push", "-q", str(bare), "main"], cwd=work, check=True)
    subprocess.run(["git", "remote", "add", "origin", str(bare)], cwd=work, check=True)
    STUB_STATE.write_text(json.dumps({"open": [], "next": 7, "bodies": {}}))
    STUB_LOG.write_text("")
    return work


# dry-run plans: PROMOTE, REVERT, and the three verdicts that propose nothing.
work = make_work()
p = run(["propose", "--verdict-file", PROMOTE, "--dry-run"], work)
assert p.returncode == 0 and "t8112 -> default-on" in p.stdout and "auto/promote-t8112" in p.stdout, p
assert "squash-merge" in p.stdout and "patch version" in p.stdout, p.stdout
p = run(["propose", "--verdict-file", REVERT, "--dry-run"], work)
assert p.returncode == 0 and "t6021 -> opt-in" in p.stdout and "urgent" in p.stdout, p
for name, f in (("CONFLICT", CONFLICT), ("STAY", STAY), ("ON", ON)):
    p = run(["propose", "--verdict-file", f, "--dry-run"], work)
    assert p.returncode == 0 and "nothing to do" in p.stdout, (name, p.stdout, p.stderr)
print("promote_from_verdict test: dry-run plans ok")

# the production repo refuses a synthetic verdict file.
p = run(["propose", "--verdict-file", PROMOTE, "--dry-run"], work,
        env={"GITHUB_REPOSITORY": "joshuaswarren/omarchy-ane"})
assert p.returncode == 1 and "refused" in p.stderr, (p.returncode, p.stderr)
print("promote_from_verdict test: production refuses synthetic verdicts")

# a real propose opens one PR; a second run updates it, never duplicates it.
work = make_work()
for i in (1, 2):
    p = run(["propose", "--verdict-file", PROMOTE], work)
    assert p.returncode == 0, (i, p.stderr)
calls = [json.loads(l) for l in STUB_LOG.read_text().splitlines()]
creates = [c for c in calls if c[0].endswith("/pulls") and c[1] == "POST"]
patches = [c for c in calls if "/pulls/" in c[0] and c[1] == "PATCH"]
assert len(creates) == 1 and len(patches) == 1, (len(creates), len(patches))
state = json.loads(STUB_STATE.read_text())
assert len(state["open"]) == 1 and state["open"][0]["number"] == 7, state["open"]
labels = state["open"][0]["labels"]
assert [l["name"] for l in labels] == ["auto-promotion"], labels
body = state["bodies"]["7"]
assert "<!-- promotion-verdict" in body and '"verdict": "PROMOTE"' in body.replace("'", '"') or \
    '"verdict": "PROMOTE"' in body, body[:200]
print("promote_from_verdict test: double propose updates PR 7, no duplicate")

# gate merges the open PR when the fresh verdict matches the recorded block.
p = run(["gate", "--pr", "7", "--verdict-file", PROMOTE], work)
assert p.returncode == 0 and "recorded=PROMOTE fresh=PROMOTE" in p.stdout, p
assert "MERGED\tt8112\tstub0merge1sha" in p.stdout, p.stdout
print("promote_from_verdict test: gate compares and merges")

# gate refuses when the judged row the PR rests on is gone (a different row landed).
CHANGED = verdict([chip("t8112", "PROMOTE", sha="cafe567890ab")])
p = run(["gate", "--pr", "7", "--verdict-file", CHANGED], work)
assert p.returncode == 1 and "judged row deadbeef1234 passed=true driver_source=None -> gone" in p.stderr, p.stderr
print("promote_from_verdict test: gate refuses a changed verdict")

# The real shape: PR #113's block (t6020 PROMOTE, targets overlay and aurora-dt,
# 42 rows, 1 judged: 3c9389040f51 in-tree pass), on an opt-in chip of this tree
# so a promotion flip keeps the suite green. gate accepts what still backs the
# PROMOTE: new unjudged rows, new judged passing rows, rows of another chip.
# It refuses a changed verdict or target, a new judged failing row, and a
# judged row that changed or went away.
OPT_CHIP = sorted(p for p, src, st in (l.split() for l in (repo / "packaging/dt/overlays").read_text().splitlines()
                                       if l and l[0] != "#") if src == f"{p}-ane.dts" and st == "opt-in")[0]
PR113 = {**json.loads((repo / "tools/fixtures/promote_from_verdict/pr113-block.json").read_text()), "chip": OPT_CHIP}
JUDGED = "3c9389040f51"


def pr7_body(block):
    s = json.loads(STUB_STATE.read_text())
    s["bodies"]["7"] = f"Automatic promotion-flow PR.\n\n<!-- promotion-verdict\n{json.dumps(block, sort_keys=True)}\n-->"
    STUB_STATE.write_text(json.dumps(s))


def fresh(rows=None, other=None, **change):
    c = {**copy.deepcopy(PR113), **change}
    if rows is not None:
        c["rows"] = rows
    return verdict([c] + ([other] if other else []))


def r(sha, judged=True, passed=True, ds="intree"):
    return {"row_sha": sha, "judged": judged, "passed": passed, "driver_source": ds,
            "reasons": [] if passed or not judged else ["smoke 20 calls, 1 not bit-exact"]}


same = PR113["rows"]
edited = lambda **kw: [{**x, **kw} if x["row_sha"] == JUDGED else x for x in same]
pr7_body(PR113)
for name, v, why in (
        ("as proposed", fresh(), None),
        ("a new unjudged row, rows in another order", fresh([*same[::-1], r("bbbbbbbbbbbb", judged=False, passed=False,
                                                                             ds=None)]), None),
        ("a new judged passing in-tree row", fresh([*same, r("cccccccccccc")]), None),
        ("a new judged passing dkms row", fresh([*same, r("dddddddddddd", ds="dkms")]), None),
        ("a judged row of another chip", fresh(other={"chip": "t8140", "state": "opt-in", "verdict": "CONFLICT",
                                                      "targets": [], "rows": [r("eeeeeeeeeeee", passed=False)]}), None),
        ("a new judged failing row (CONFLICT)", fresh([*same, r("ffffffffffff", passed=False)], verdict="CONFLICT",
                                                      targets=[]), "verdict PROMOTE -> CONFLICT"),
        ("a new judged failing row, same verdict", fresh([*same, r("ffffffffffff", passed=False)]),
         "new judged failing row ffffffffffff"),
        ("the judged row now fails", fresh(edited(passed=False)),
         f"judged row {JUDGED} passed=true driver_source=intree -> passed=false driver_source=intree"),
        ("the judged row's driver_source changed", fresh(edited(driver_source="dkms")),
         f"judged row {JUDGED} passed=true driver_source=intree -> passed=true driver_source=dkms"),
        ("the judged row is gone", fresh([x for x in same if x["row_sha"] != JUDGED]),
         f"judged row {JUDGED} passed=true driver_source=intree -> gone"),
        ("the targets changed", fresh(targets=["overlay"]), "targets ['overlay', 'aurora-dt'] -> ['overlay']"),
        ("the verdict a flipped tree gives (run 37146125714)", fresh(state="on", verdict="ON", targets=["aurora-dt"]),
         "verdict PROMOTE -> ON")):
    p = run(["gate", "--pr", "7", "--verdict-file", v], work)
    if why is None:
        assert p.returncode == 0 and f"MERGED\t{OPT_CHIP}\tstub0merge1sha" in p.stdout, (name, p.stdout, p.stderr)
    else:
        assert p.returncode == 1 and why in p.stderr and "MERGED" not in p.stdout, (name, p.stderr)

# gate refuses to run on a checkout that already carries the PR's flip (the PR
# head): there the chip is enabled, and any fresh verdict judges the flipped tree.
subprocess.run([sys.executable, "tools/promote_chip.py", "--chip", OPT_CHIP, "--to", "default-on", "--apply"],
               cwd=work, check=True, capture_output=True)
STUB_LOG.write_text("")
p = run(["gate", "--pr", "7", "--verdict-file", fresh()], work)
assert p.returncode == 1 and f"already has {OPT_CHIP} enabled, the PR's flip" in p.stderr, p.stderr
assert not [c for c in (json.loads(l) for l in STUB_LOG.read_text().splitlines()) if c[0].endswith("/merge")]
subprocess.run(["git", "-C", str(work), "checkout", "-q", "--", "."], check=True)
print("promote_from_verdict test: gate accepts more support, refuses changed evidence and a flipped checkout")

# propose closes an open auto-promotion PR whose chip it no longer proposes,
# and rewrites the block of a PR it does propose, legacy body or not. Only its
# own PRs: labeled auto-promotion, from this repository.
own, fork = {"full_name": "t/repo"}, {"full_name": "someone/fork"}
label = [{"name": "auto-promotion"}]
s = json.loads(STUB_STATE.read_text())
s["open"] = [{"number": 7, "labels": label, "head": {"ref": "auto/promote-t8112", "repo": own}},
             {"number": 5, "labels": label, "head": {"ref": "auto/promote-t6000", "repo": own}},
             {"number": 6, "labels": label, "head": {"ref": "auto/promote-t6002", "repo": fork}},
             {"number": 3, "labels": [], "head": {"ref": "auto/promote-t6022", "repo": own}},
             {"number": 4, "labels": [], "head": {"ref": "someone/else", "repo": own}}]
s["bodies"]["7"] = "A legacy body with no promotion-verdict block."
STUB_STATE.write_text(json.dumps(s))
STUB_LOG.write_text("")
p = run(["propose", "--verdict-file", verdict([chip("t8112", "PROMOTE"), chip("t6000", "CONFLICT", passed=False,
                                                                           reasons=["x"])])], work)
assert p.returncode == 0 and "updated PR #7" in p.stdout and "closed stale PR #5 (t6000 is CONFLICT)" in p.stdout, p
calls = [json.loads(l) for l in STUB_LOG.read_text().splitlines()]
assert ["repos/t/repo/pulls/5", "PATCH", {"state": "closed"}] in calls, calls
assert any(c[0] == "repos/t/repo/issues/5/comments" and "t6000 is CONFLICT" in c[2]["body"] for c in calls), calls
assert not [c for c in calls if re.search(r"/(pulls|issues)/[346](/|$)", c[0])], "not our flip PR: left alone"
s = json.loads(STUB_STATE.read_text())
assert [x["number"] for x in s["open"]] == [7, 6, 3, 4], s["open"]
block7 = json.loads(s["bodies"]["7"].split("<!-- promotion-verdict\n", 1)[1].split("\n-->", 1)[0])
assert block7["chip"] == "t8112" and block7["verdict"] == "PROMOTE", s["bodies"]["7"]
print("promote_from_verdict test: propose closes a stale flip PR and rewrites a legacy block")

# the release plan: next patch version from the tags, no writes in dry-run.
subprocess.run(["git", "-C", str(work), "tag", "v0.4.0"], check=True)
before = (work / "CHANGELOG.md").read_text()
p = run(["release", "--chip", "t8112", "--merge-sha", "head", "--verdict-file", PROMOTE,
         "--dry-run"], work)
assert p.returncode == 0 and "X.Y.Z" in p.stdout, p.stdout
assert (work / "CHANGELOG.md").read_text() == before
print("promote_from_verdict test: release plan ok")

# aurora-plan and aurora-pr: the aurora-silicon/linux PR for a PROMOTE whose
# passing row is in-tree, on a fake t8112-shaped aurora tree (git, so the base
# commit is known).
def aurora_tree(status):
    t = Path(tempfile.mkdtemp())
    dts = t / "arch/arm64/boot/dts/apple"
    dts.mkdir(parents=True)
    (t / "scripts/dtc/include-prefixes").mkdir(parents=True)
    (dts / "t8112.dtsi").write_text(
        '/ { compatible = "apple,t8112"; #address-cells = <2>; #size-cells = <2>;\n'
        '  soc { #address-cells = <2>; #size-cells = <2>; ranges;\n'
        f'    ane_dart0: iommu@300 {{ #iommu-cells = <1>; status = "{status}"; }};\n'
        f'    ane: ane@400 {{ compatible = "apple,t8112-ane"; iommus = <&ane_dart0 0>; status = "{status}"; }};\n'
        '  };\n};\n')
    (dts / "t8112-j413.dts").write_text('/dts-v1/;\n#include "t8112.dtsi"\n')
    subprocess.run(["git", "init", "-q", str(t)], check=True)
    subprocess.run(["git", "-C", str(t), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(t), "-c", "user.name=Joshua Warren",
                    "-c", "user.email=816217+joshuaswarren@users.noreply.github.com", "commit", "-qm", "base"],
                   check=True)
    return t


def writes():
    return [(ep, m) for ep, m, _ in (json.loads(l) for l in STUB_LOG.read_text().splitlines()) if m != "GET"]


# The PR step runs with build tools that record any call: it must build nothing.
TRAP, TRAP_LOG = Path(tempfile.mkdtemp()), Path(tempfile.mkdtemp()) / "trap.log"
for tool in ("gcc", "cpp", "cc", "dtc", "fdtoverlay", "make", "flex", "bison"):
    (TRAP / tool).write_text(f"#!/bin/sh\necho {tool} >> {TRAP_LOG}\nexit 1\n")
    (TRAP / tool).chmod(0o755)
PR_ENV = {"PATH": f"{TRAP}:{STUB.parent}:{os.environ['PATH']}"}

INTREE_PROMOTE = verdict([{"chip": "t8112", "state": "opt-in", "verdict": "PROMOTE",
                           "targets": ["overlay", "aurora-dt"],
                           "rows": [{"row_sha": "1a2b3c4d5e6f", "judged": True, "driver_source": "intree",
                                     "passed": True, "reasons": []}]}])
work = make_work()
tree = aurora_tree("disabled")
base = subprocess.run(["git", "-C", str(tree), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
old = (tree / "arch/arm64/boot/dts/apple/t8112.dtsi").read_text()
plan_file = FIXTURES / "aurora-plan.json"

# No aurora-dt target: an empty plan, and aurora-pr has nothing to do.
p = run(["aurora-plan", "--tree", str(tree), "--out", str(plan_file), "--verdict-file", PROMOTE], work)
assert p.returncode == 0 and "nothing to do (no chip with the aurora-dt target)" in p.stdout, p
assert json.loads(plan_file.read_text())["chips"] == [] and "PLANNED" not in p.stdout
p = run(["aurora-pr", "--plan", str(plan_file), "--verdict-file", INTREE_PROMOTE], work, env=PR_ENV)
assert p.returncode == 0 and "the plan has no chip; nothing to do" in p.stdout, p

# The plan: built from the tree, one JSON line, and no GitHub call (no token).
p = run(["aurora-plan", "--tree", str(tree), "--out", str(plan_file), "--verdict-file", INTREE_PROMOTE], work,
        env={"GH_TOKEN": ""})
assert p.returncode == 0 and "PLANNED\tt8112\n" in p.stdout and "+&ane_dart0 {" in p.stdout, p
assert STUB_LOG.read_text() == "", "aurora-plan calls no GitHub API"
assert len(plan_file.read_text().splitlines()) == 1
plan = json.loads(plan_file.read_text())
new = old.rstrip("\n") + ('\n\n/* The ANE has run on the M2: enable it and the nodes it uses. */\n&ane {\n\tstatus = "okay";\n'
                          '};\n\n&ane_dart0 {\n\tstatus = "okay";\n};\n')
assert plan["base"] == base and plan["chips"][0]["refs"] == ["&ane", "&ane_dart0"], plan
assert plan["chips"][0]["sha256"] == hashlib.sha256(new.encode()).hexdigest(), plan
assert subprocess.run(["git", "-C", str(tree), "status", "--porcelain"], capture_output=True,
                      text=True).stdout == "", "aurora-plan writes nothing in the tree"

# The dry run prints the PR and calls nothing.
p = run(["aurora-pr", "--plan", str(plan_file), "--verdict-file", INTREE_PROMOTE, "--dry-run"], work, env=PR_ENV)
assert p.returncode == 0, p.stderr
assert "branch joshuaswarren/aurorasilicon-linux:omarchy-ane/enable-t8112-ane" in p.stdout, p.stdout
assert "+&ane_dart0 {" in p.stdout and "--- title\narm64: dts: apple: t8112: Enable the ANE\n" in p.stdout, p.stdout
assert "1a2b3c4d5e6f passed the omarchy-ane promotion rule" in " ".join(p.stdout.split()), p.stdout
assert "--- comment\n@iconidentify In-tree row(s) 1a2b3c4d5e6f on the M2 (T8112) passed" in p.stdout, p.stdout
assert "Signed-off-by: Joshua Warren <816217+joshuaswarren@users.noreply.github.com>" in p.stdout, p.stdout
assert STUB_LOG.read_text() == "", "a dry run calls nothing"
print("promote_from_verdict test: aurora-plan calls no API; aurora-pr dry run ok")


def stub_state(**kw):
    s = json.loads(STUB_STATE.read_text())
    s.update(kw)
    STUB_STATE.write_text(json.dumps(s))


def pr_run():
    return run(["aurora-pr", "--plan", str(plan_file), "--verdict-file", INTREE_PROMOTE], work, env=PR_ENV)


fork, up = "repos/joshuaswarren/aurorasilicon-linux", "repos/aurora-silicon/linux"

# The base tip moved (or the checkout was not aurora-wip's tip): refused
# before any write.
stub_state(tip="f" * 40, file=old)
p = pr_run()
assert p.returncode == 1 and f"is at ffffffffffff, but the plan was built on {base[:12]}" in p.stderr, p
assert writes() == [], writes()

# A plan that does not match the file at the base, or a change this tool
# never makes: refused before any write.
stub_state(tip=base)
good_plan = plan_file.read_text()
for name, change, why in (
        ("sha256", lambda e: e.update(sha256="0" * 64), "does not have the plan's sha256"),
        ("ref", lambda e: e.update(refs=['&ane { compatible = "x"; }; //']), "not a change this tool makes"),
        ("file", lambda e: e.update(file="arch/arm64/boot/dts/apple/t8103.dtsi"), "not one aurora-plan writes"),
        ("chip", lambda e: e.update(chip="t6000"), "not a PROMOTE chip with the aurora-dt target")):
    bad = json.loads(good_plan)
    change(bad["chips"][0])
    plan_file.write_text(json.dumps(bad))
    p = pr_run()
    assert p.returncode == 1 and why in p.stderr, (name, p.stderr)
    assert writes() == [], (name, writes())
plan_file.write_text(good_plan)
stub_state(file=old.replace("ane_dart0", "ane_dart9"))
p = pr_run()
assert p.returncode == 1 and "does not have the plan's sha256" in p.stderr and writes() == [], p
stub_state(file=old)
print("promote_from_verdict test: aurora-pr refuses a moved base and a plan that does not match, before any write")

# Live runs against the stub. The first comment POST fails: the run fails
# after opening the PR. The next run updates the PR and posts the comment;
# the third finds the comment's marker and posts nothing.
stub_state(fail_comment=True)
p = pr_run()
assert p.returncode == 1 and "comment refused" in p.stderr, p
for i in (2, 3):
    p = pr_run()
    assert p.returncode == 0 and "AURORA\t900\tt8112\tstubcommit" in p.stdout, (i, p.stdout, p.stderr)
assert "already has the comment for #900" in p.stdout, p.stdout
comment = [f"{up}/issues/155/comments", "POST"]
assert writes() == [(f"{fork}/git/trees", "POST"), (f"{fork}/git/commits", "POST"), (f"{fork}/git/refs", "POST"),
                    (f"{up}/pulls", "POST"), tuple(comment),
                    (f"{fork}/git/trees", "POST"), (f"{fork}/git/commits", "POST"),
                    (f"{fork}/git/refs/heads/omarchy-ane/enable-t8112-ane", "PATCH"), (f"{up}/pulls/900", "PATCH"),
                    tuple(comment),
                    (f"{fork}/git/trees", "POST"), (f"{fork}/git/commits", "POST"),
                    (f"{fork}/git/refs/heads/omarchy-ane/enable-t8112-ane", "PATCH"), (f"{up}/pulls/900", "PATCH")], \
    writes()
calls = [json.loads(l) for l in STUB_LOG.read_text().splitlines()]
posted = json.loads(STUB_STATE.read_text())["comments"]
assert len(posted) == 1 and posted[0]["body"].startswith("@iconidentify "), posted
assert "https://github.com/aurora-silicon/linux/pull/900" in posted[0]["body"]
assert posted[0]["body"].endswith("<!-- omarchy-ane aurora-dt t8112 aurora-silicon/linux#900 -->"), posted
# The commit sits on the checked tip; its file is the one read from GitHub at
# that commit, changed again here, not the plan's.
tree_post = next(b for ep, m, b in calls if ep.endswith("/git/trees"))
assert tree_post["base_tree"] == "stubbasetree" and tree_post["tree"][0]["content"] == new, tree_post
commit_post = next(b for ep, m, b in calls if ep.endswith("/git/commits") and m == "POST")
assert commit_post["parents"] == [base] and commit_post["author"]["name"] == "Joshua Warren", commit_post
pr = next(b for ep, m, b in calls if ep == f"{up}/pulls")
assert pr["head"] == "joshuaswarren:omarchy-ane/enable-t8112-ane" and pr["base"] == "aurora-wip", pr
assert not TRAP_LOG.exists(), f"aurora-pr ran a build tool: {TRAP_LOG.read_text()}"
print("promote_from_verdict test: aurora PR opened once, then updated; the comment posted once, after a failure")

# Already enabled at the base: no plan entry.
STUB_LOG.write_text("")
p = run(["aurora-plan", "--tree", str(aurora_tree("okay")), "--out", str(plan_file), "--verdict-file",
         INTREE_PROMOTE], work)
assert p.returncode == 0 and "already enables the ANE; no PR" in p.stdout, p
assert json.loads(plan_file.read_text())["chips"] == [] and STUB_LOG.read_text() == ""


def plan_lines(p):
    return [l for l in p.stdout.splitlines() if l.startswith(("PLANNED", "SKIPPED"))]


# No board of the chip has an ANE node (the base predates the in-tree driver,
# as aurora-wip did before #155): SKIPPED, exit 0, no plan entry, so the
# aurora-pr job does not run. Another chip in the same verdict still plans.
TWO_PROMOTE = verdict([{**json.loads(Path(INTREE_PROMOTE).read_text())["chips"][0], "chip": c}
                       for c in ("t6000", "t8112")])
mixed = aurora_tree("disabled")
dts = mixed / "arch/arm64/boot/dts/apple"
(dts / "t6000.dtsi").write_text('/ { compatible = "apple,t6000"; soc { pmgr@100 { status = "disabled"; }; }; };\n')
(dts / "t6000-j314s.dts").write_text('/dts-v1/;\n#include "t6000.dtsi"\n')
p = run(["aurora-plan", "--tree", str(mixed), "--out", str(plan_file), "--verdict-file", TWO_PROMOTE], work)
assert p.returncode == 0, p
lines = plan_lines(p)
assert [l.split("\t")[:2] for l in lines] == [["SKIPPED", "t6000"], ["PLANNED", "t8112"]], lines
assert "no t6000 board has an apple,*-ane node at this tree (t6000-j314s.dts)" in lines[0], lines
assert "only after the in-tree driver lands there; the overlay PR is not affected" in lines[0], lines
assert [e["chip"] for e in json.loads(plan_file.read_text())["chips"]] == ["t8112"]
bare = aurora_tree("okay")
(bare / "arch/arm64/boot/dts/apple/t8112.dtsi").write_text('/ { compatible = "apple,t8112"; };\n')
p = run(["aurora-plan", "--tree", str(bare), "--out", str(plan_file), "--verdict-file", INTREE_PROMOTE], work)
assert p.returncode == 0 and [l.split("\t")[:2] for l in plan_lines(p)] == [["SKIPPED", "t8112"]], p
assert json.loads(plan_file.read_text())["chips"] == [] and STUB_LOG.read_text() == "", p

# Every other refusal still fails the plan: a tree that does not build, and a
# chip with the node on one board but not on another.
broken = aurora_tree("disabled")
(broken / "arch/arm64/boot/dts/apple/t8112.dtsi").write_text('/ { compatible = "apple,t8112"; soc { ane@400 {\n')
p = run(["aurora-plan", "--tree", str(broken), "--out", str(plan_file), "--verdict-file", INTREE_PROMOTE], work)
assert p.returncode == 1 and "aurora-plan: t8112: dtc failed on t8112-j413.dts" in p.stderr and not plan_lines(p), p
partial = aurora_tree("disabled")
dts = partial / "arch/arm64/boot/dts/apple"
(dts / "t8112.dtsi").write_text('/ { compatible = "apple,t8112"; #address-cells = <2>; #size-cells = <2>;\n'
                                '  soc { #address-cells = <2>; #size-cells = <2>; ranges; }; };\n')
(dts / "t8112-j415.dts").write_text('/dts-v1/;\n#include "t8112.dtsi"\n'
                                    '/ { soc { ane@400 { compatible = "apple,t8112-ane"; status = "disabled"; }; }; };\n')
p = run(["aurora-plan", "--tree", str(partial), "--out", str(plan_file), "--verdict-file", INTREE_PROMOTE], work)
assert p.returncode == 1 and "t8112-j413.dts has no apple,*-ane node at this tree, but another t8112 board has one" \
    in p.stderr and not plan_lines(p), p
print("promote_from_verdict test: aurora-plan skips a chip without a node, plans one with it, fails a broken tree")

# A chip that is already on by default in omarchy-ane, with a passing in-tree
# row: promotion_check gives it the aurora-dt target alone. The aurora tree
# decides: a disabled node is planned, an enabled node is a no-op, and no node
# is SKIPPED. A verdict without the target (no in-tree pass) plans nothing.
on_verdict = {"chip": "t8112", "state": "on", "verdict": "ON", "targets": ["aurora-dt"],
              "rows": [{"row_sha": "1a2b3c4d5e6f", "judged": True, "driver_source": "intree", "passed": True,
                        "reasons": []}]}
INTREE_ON, DKMS_ON = verdict([on_verdict]), verdict([{**on_verdict, "targets": []}])
for name, t, want in (("disabled", tree, "PLANNED"), ("enabled", aurora_tree("okay"), None), ("no node", bare, "SKIPPED")):
    p = run(["aurora-plan", "--tree", str(t), "--out", str(plan_file), "--verdict-file", INTREE_ON], work)
    assert p.returncode == 0 and [l.split("\t")[:2] for l in plan_lines(p)] == ([[want, "t8112"]] if want else []), \
        (name, p.stdout, p.stderr)
    assert ("already enables the ANE; no PR" in p.stdout) == (want is None), (name, p.stdout)
    assert [e["chip"] for e in json.loads(plan_file.read_text())["chips"]] == (["t8112"] if want == "PLANNED" else [])
p = run(["aurora-plan", "--tree", str(tree), "--out", str(plan_file), "--verdict-file", DKMS_ON], work)
assert p.returncode == 0 and "nothing to do (no chip with the aurora-dt target)" in p.stdout and not plan_lines(p), p

# Its PR text names no auto/promote PR, and a run reuses the chip's branch, PR
# and #155 marker: it updates PR 900 and posts no second comment.
run(["aurora-plan", "--tree", str(tree), "--out", str(plan_file), "--verdict-file", INTREE_ON], work)
p = run(["aurora-pr", "--plan", str(plan_file), "--verdict-file", INTREE_ON, "--dry-run"], work, env=PR_ENV)
assert p.returncode == 0 and "omarchy-ane already enables the T8112 overlay by default." in p.stdout, p
assert "auto/promote-t8112" not in p.stdout, p.stdout
STUB_LOG.write_text("")
p = run(["aurora-pr", "--plan", str(plan_file), "--verdict-file", INTREE_ON], work, env=PR_ENV)
assert p.returncode == 0 and "updated aurora-silicon/linux#900" in p.stdout, (p.stdout, p.stderr)
assert "already has the comment for #900" in p.stdout and (f"{up}/issues/155/comments", "POST") not in writes(), p
print("promote_from_verdict test: an ON chip with an in-tree pass plans, no-ops or skips by the aurora tree")

# The workflow: AURORA_PR_TOKEN reaches one job, and that job builds nothing;
# the job that builds the aurora tree holds no credential.
wf = (repo / ".github/workflows/promotion.yml").read_text().split("\njobs:\n", 1)[1]
jobs = {j.split(":", 1)[0]: j for j in re.split(r"(?m)^  (?=[a-z][a-z0-9-]*:$)", wf) if j.strip()}
assert [n for n, j in jobs.items() if "AURORA_PR_TOKEN" in j] == ["aurora-pr"], list(jobs)
for word in ("asahi-dtbs", "aurora-plan --tree", "aurora_dt.py", "make ", "gcc", "dtc", "apt-get"):
    assert word not in jobs["aurora-pr"], word
plan_job = jobs["aurora-plan"]
assert "tools/asahi-dtbs" in plan_job and "persist-credentials: false" in plan_job, plan_job
for word in ("secrets.", "github.token", "GH_TOKEN", "upload-artifact"):
    assert word not in plan_job, word
for name in ("aurora-plan", "aurora-pr"):
    assert "    permissions:\n      contents: read\n" in jobs[name], name
print("promote_from_verdict test: the token reaches only the aurora-pr job, which builds nothing")

print("promote_from_verdict test: ok")
