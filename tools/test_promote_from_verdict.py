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
if "pulls?state=open" in ep:
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

print("promote_from_verdict test: ok")
