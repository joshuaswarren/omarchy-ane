#!/usr/bin/env python3
"""Offline checks for tools/promote_from_verdict.py: the dry-run plans, the
production refusal of a synthetic verdict file, a real propose double-run
against a stub gh (second run updates the PR, never duplicates it), the gate
compare-and-merge on the recorded verdict block, its refusal on a changed
verdict, and the release plan. No network; gh is a stub."""
import json
import os
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
import json, os, sys
a = sys.argv[1:]
ep = a[1] if a[0] == "api" else a[0]
method = a[a.index("-X") + 1] if "-X" in a else "GET"
body = json.loads(sys.stdin.read()) if "--input" in a else None
s = json.load(open(os.environ["STUB_STATE"]))
open(os.environ["STUB_LOG"], "a").write(json.dumps([ep, method, body]) + "\\n")
def out(o): print(json.dumps(o))
if ep.startswith("repos/aurora-silicon/linux/") or "/git/" in ep:
    if "/git/trees" in ep: out({{"sha": "stubtree"}})
    elif "/git/commits" in ep: out({{"sha": "stubcommit"}})
    elif "/git/matching-refs/" in ep: out(s.get("refs", []))
    elif ep.endswith("/git/refs"): s["refs"] = [{{"ref": body["ref"]}}]; out({{}})
    elif "pulls?state=open" in ep: out(s.get("aurora", []))
    elif ep.endswith("/pulls"):
        s["aurora"] = [{{"number": 900}}]
        out({{"number": 900, "html_url": "https://github.com/aurora-silicon/linux/pull/900"}})
    else: out({{}})
elif "pulls?state=open" in ep:
    out(s["open"])
elif ep.endswith("/pulls") and method == "POST":
    n = s["next"]; s["next"] += 1
    s["open"] = s["open"] + [{{"number": n, "labels": [], "head": {{"ref": body["head"]}}}}]
    s["bodies"][str(n)] = body["body"]
    out({{"number": n, "body": body["body"]}})
elif "/pulls/" in ep and method == "PATCH":
    n = ep.rsplit("/", 1)[1]; s["bodies"][n] = body["body"]; out({{"number": int(n)}})
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

# gate refuses when the fresh verdict changed (a new row landed).
CHANGED = verdict([chip("t8112", "PROMOTE", sha="cafe567890ab")])
p = run(["gate", "--pr", "7", "--verdict-file", CHANGED], work)
assert p.returncode == 1 and "fresh verdict differs" in p.stderr, (p.returncode, p.stderr)
print("promote_from_verdict test: gate refuses a changed verdict")

# the release plan: next patch version from the tags, no writes in dry-run.
subprocess.run(["git", "-C", str(work), "tag", "v0.4.0"], check=True)
before = (work / "CHANGELOG.md").read_text()
p = run(["release", "--chip", "t8112", "--merge-sha", "head", "--verdict-file", PROMOTE,
         "--dry-run"], work)
assert p.returncode == 0 and "X.Y.Z" in p.stdout, p.stdout
assert (work / "CHANGELOG.md").read_text() == before
print("promote_from_verdict test: release plan ok")

# aurora: the aurora-silicon/linux PR for a PROMOTE whose passing row is
# in-tree, on a fake t8112-shaped aurora tree (git, so the base commit is known).
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


INTREE_PROMOTE = verdict([{"chip": "t8112", "state": "opt-in", "verdict": "PROMOTE",
                           "targets": ["overlay", "aurora-dt"],
                           "rows": [{"row_sha": "1a2b3c4d5e6f", "judged": True, "driver_source": "intree",
                                     "passed": True, "reasons": []}]}])
work = make_work()
tree = aurora_tree("disabled")
p = run(["aurora", "--tree", str(tree), "--verdict-file", PROMOTE, "--dry-run"], work)
assert p.returncode == 0 and "nothing to do (no PROMOTE chip with a passing in-tree row)" in p.stdout, p
p = run(["aurora", "--tree", str(tree), "--verdict-file", INTREE_PROMOTE, "--dry-run"], work)
assert p.returncode == 0, p.stderr
assert "branch joshuaswarren/aurorasilicon-linux:omarchy-ane/enable-t8112-ane" in p.stdout, p.stdout
assert "+&ane_dart0 {" in p.stdout and "--- title\narm64: dts: apple: t8112: Enable the ANE\n" in p.stdout, p.stdout
assert "1a2b3c4d5e6f passed the omarchy-ane promotion rule" in " ".join(p.stdout.split()), p.stdout
assert "--- comment\n@iconidentify In-tree row(s) 1a2b3c4d5e6f on the M2 (T8112) passed" in p.stdout, p.stdout
assert "Signed-off-by: Joshua Warren <816217+joshuaswarren@users.noreply.github.com>" in p.stdout, p.stdout
assert STUB_LOG.read_text() == "", "a dry run calls nothing"
assert subprocess.run(["git", "-C", str(tree), "status", "--porcelain"], capture_output=True,
                      text=True).stdout == "", "a dry run writes nothing"
print("promote_from_verdict test: aurora dry run ok")

# A live run against the stub: one commit, one ref, one PR, one comment for
# the in-tree tester; a second run moves the ref and updates the same PR.
for i in (1, 2):
    p = run(["aurora", "--tree", str(tree), "--verdict-file", INTREE_PROMOTE], work)
    assert p.returncode == 0 and "AURORA\t900\tt8112\tstubcommit" in p.stdout, (i, p.stdout, p.stderr)
calls = [json.loads(l) for l in STUB_LOG.read_text().splitlines()]
fork, up = "repos/joshuaswarren/aurorasilicon-linux", "repos/aurora-silicon/linux"
writes = [(ep, m) for ep, m, _ in calls if m != "GET"]
assert writes == [(f"{fork}/git/trees", "POST"), (f"{fork}/git/commits", "POST"), (f"{fork}/git/refs", "POST"),
                  (f"{up}/pulls", "POST"), (f"{up}/issues/155/comments", "POST"),
                  (f"{fork}/git/trees", "POST"), (f"{fork}/git/commits", "POST"),
                  (f"{fork}/git/refs/heads/omarchy-ane/enable-t8112-ane", "PATCH"),
                  (f"{up}/pulls/900", "PATCH")], writes
base = subprocess.run(["git", "-C", str(tree), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
commit = next(b for ep, m, b in calls if ep.endswith("/git/commits"))
assert commit["parents"] == [base] and commit["author"]["name"] == "Joshua Warren", commit
pr = next(b for ep, m, b in calls if ep == f"{up}/pulls")
assert pr["head"] == "joshuaswarren:omarchy-ane/enable-t8112-ane" and pr["base"] == "aurora-wip", pr
comment = next(b for ep, m, b in calls if ep.endswith("/comments"))["body"]
assert comment.startswith("@iconidentify ") and "https://github.com/aurora-silicon/linux/pull/900" in comment
print("promote_from_verdict test: aurora PR opened once, then updated")

# Already enabled at the base: no PR. No ANE node there: the run fails.
STUB_LOG.write_text("")
p = run(["aurora", "--tree", str(aurora_tree("okay")), "--verdict-file", INTREE_PROMOTE], work)
assert p.returncode == 0 and "already enables the ANE; no PR" in p.stdout and STUB_LOG.read_text() == "", p
bare = aurora_tree("okay")
(bare / "arch/arm64/boot/dts/apple/t8112.dtsi").write_text('/ { compatible = "apple,t8112"; };\n')
p = run(["aurora", "--tree", str(bare), "--verdict-file", INTREE_PROMOTE], work)
assert p.returncode == 1 and "has no apple,*-ane node" in p.stderr and STUB_LOG.read_text() == "", p
print("promote_from_verdict test: aurora skips an enabled chip, fails without a node")

print("promote_from_verdict test: ok")
