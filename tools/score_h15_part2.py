#!/usr/bin/env python3
"""Score one ane_h15 stage 5 part 2 output directory into outcome A to E, or UNSCORED.

usage: score_h15_part2.py OUTDIR      (the folder the part 2 wrapper wrote; reads files only, runs nothing)

Reads wrapper.log, ane_h15-dmesg.txt, data-before.txt, data-after.txt and, when present, compare.txt. The page comparison
is recomputed from the two hash files and must agree with compare.txt. Anything missing, inconsistent or unexpected is
UNSCORED with the reason; it is never mapped to a letter by guessing.

The facts come from the run that the last RESULT line closes. A capture can hold several runs, and what an earlier run
logged says nothing about the last one: a stale message must not send a silent run to A, and a gate line from an earlier
run must not open this one. When the capture cannot be attributed to a single run, it stays UNSCORED.

A  the module logged a mailbox message                       B  no message, hashed data pages changed
C  no message, data unchanged, the gate opened/forced/was open   D  the power gate never opened
E  the job never finished (machine went down)
"""
import re
import sys
from pathlib import Path

NEXT = {
    "A": "Stage 6, one run, no other change. It writes the two mailbox send registers for the first time on an H15 part: say so in the ask.",
    "B": "The firmware probably executed (a net change in the hashed range, nothing more). The mailbox transport or the interrupt wiring is the next suspect. Rerun stage 5 on a build that logs the post-wait status line; do not widen the read guard.",
    "C": "The power gate is not the missing step on this chip, or the firmware stalls earlier. Try pwgate_t6031=2 (always write 0 first) before any other offset; the others are hypotheses only.",
    "D": "The core never started and no reboot is needed. Log the window word, read it once more after a long wait, then check the power domain status registers before any new write.",
    "E": "Stop. Do not retry. Ask for the last line of the per-line log; a crash stops the run for a fix.",
}
RESULT = re.compile(r"ane_h15 RESULT stage=5 soc=\S+ verdict=(\S+) reason=(.*)")
END = re.compile(r"h15 part2 wrapper end .* insmod_rc=(\d+) second_hash_rc=(\d+)")
START = re.compile(r"^h15 part2 wrapper start .*$", re.MULTILINE)
SUMMARY = re.compile(r"run started_seen=[^\n]*?\bmessages=(\d+)")
MESSAGE = re.compile(r"ane_h15 msg\d+ raw0=")
REBOOT = "The core did not stop, so the module is pinned and the machine needs a reboot before the next run. "


def unscored(why, **extra):
    return {"branch": "UNSCORED", "why": why, "next": "Not a branch: resolve the reason first, then rescore.", **extra}


def pages(path):
    """Page index -> hash for one hash file. A row that is not an index and a hash is an error, not a guess."""
    out = {}
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 2:
            raise ValueError(f"{path.name}: {line.strip()!r} is not a page index and a hash")
        index = int(fields[0])
        if index in out:
            raise ValueError(f"{path.name}: duplicate page index {index}")
        out[index] = fields[1]
    return out


def gate_state(log):
    """What the log says about the power gate: the RESULT suffix, or the gate lines the module writes itself."""
    suffix = re.search(r"pwgate=(already-open|opened|forced)", log)
    if suffix:
        return suffix.group(1)
    if "pwgate already open" in log:
        return "already-open"
    if "pwgate open after" in log:
        return "forced" if "(forced, the unconditional macOS order)" in log else "opened"
    return None


def log_facts(own, reason):
    """What the module logged for this run, without judging it."""
    info = {"pwgate": gate_state(own), "reboot_required": "core-not-stopped" in reason}
    post = re.search(r"post-wait cpu_status=(\S+) a2i_ctrl=(\S+) i2a_ctrl=(\S+)", own)
    if post:
        info["post_wait"] = dict(zip(("cpu_status", "a2i_ctrl", "i2a_ctrl"), post.groups()))
    return info


def compare_count(out):
    """The differing page count compare.txt states, or None when the file is absent. A file that is there but states no
    count is an error, not an absent optional file."""
    compare = out / "compare.txt"
    if not compare.exists():
        return None
    listed = re.fullmatch(r"pages before \d+, after \d+, differing (\d+)", compare.read_text().partition("\n")[0])
    if not listed:
        raise ValueError("compare.txt states no differing page count")
    return int(listed.group(1))


def changed_pages(out):
    before, after = pages(out / "data-before.txt"), pages(out / "data-after.txt")
    if not before or before.keys() != after.keys():
        raise ValueError(f"the two hashes cover different pages ({len(before)} before, {len(after)} after)")
    diff = sorted(k for k in before if before[k] != after[k])
    stated = compare_count(out)
    if stated is not None and stated != len(diff):
        raise ValueError(f"compare.txt says {stated} differing pages, the hashes say {len(diff)}")
    return diff


def classify_data(out, info):
    """B or C from the two page hashes, or UNSCORED when they cannot be trusted."""
    try:
        diff = changed_pages(out)
    except (OSError, ValueError) as error:
        return unscored(f"unreadable page comparison: {error}", **info)
    info["differing_pages"] = diff
    if diff:
        return {"branch": "B", "why": f"{len(diff)} hashed page(s) changed, no message", "next": NEXT["B"], **info}
    if not info["pwgate"]:
        return unscored("data unchanged but the log has no pwgate line to say whether the gate opened", **info)
    next_step = NEXT["C"]
    if info["reboot_required"]:
        next_step = REBOOT + next_step
    return {"branch": "C", "why": f"data unchanged, no message, pwgate {info['pwgate']}", "next": next_step, **info}


def no_result(dmesg, insmod_rc):
    """The module logged no RESULT line: say whether it never ran, or ran without saying so."""
    lines = [line for line in dmesg.splitlines() if "ane_h15" in line]
    if [line for line in lines if "taints kernel" not in line]:
        return unscored("no RESULT line from the module")
    if insmod_rc != 0:
        return unscored(f"the module did not come up (insmod rc={insmod_rc}), so the capture does not say why it never ran")
    if not lines:
        return unscored("the capture holds no module line at all, so the run cannot be judged")
    return unscored("the module loaded but never ran: its only kernel line is the taint notice, so probe() was never "
                    "called and no device-tree node matched; unchanged data says nothing about the firmware", never_probed=True,
                    next="Put the ANE device-tree node on the boot (the opt-in overlay; the DART variant for map_fw=1), make the job "
                         "refuse before loading when the node is absent, then run part 2 again.")


def run_facts(dmesg, results):
    chosen = results[-1]
    own = dmesg[:chosen.end()]
    if len(results) > 1:
        own = own[results[-2].end():]
    tail = dmesg[chosen.end():]
    if any("ane_h15" in line and "firmware started: do NOT unload; reboot to park the ANE" not in line
           for line in tail.splitlines()):
        raise ValueError("the capture contains later module activity without its RESULT")
    summaries = list(SUMMARY.finditer(own))
    if len(summaries) > 1:
        raise ValueError("the last RESULT cannot be attributed to a single run")
    messages = int(summaries[0].group(1)) if summaries else 0
    logged = len(MESSAGE.findall(own[:summaries[0].start()] if summaries else own))
    if logged != messages:
        raise ValueError("the log's message lines and its run summary disagree")
    verdict, reason = chosen.group(1), chosen.group(2).strip()
    info = log_facts(own, reason)
    info["messages"] = messages
    return verdict, reason, info


def wrapper_end(wrapper):
    starts = list(START.finditer(wrapper))
    if not starts:
        return None
    return END.search(wrapper, starts[-1].end())


def score(out):
    out = Path(out)
    try:
        wrapper = (out / "wrapper.log").read_text()
        dmesg = (out / "ane_h15-dmesg.txt").read_text()
    except OSError as error:
        return unscored(f"missing input: {error.filename}")
    ended = wrapper_end(wrapper)
    if not ended:
        return {"branch": "E", "why": "the wrapper never reached its end line", "next": NEXT["E"]}
    results = list(RESULT.finditer(dmesg))
    if not results:
        return no_result(dmesg, int(ended.group(1)))
    try:
        verdict, reason, info = run_facts(dmesg, results)
    except ValueError as error:
        return unscored(str(error))
    if verdict == "REFUSED":
        return unscored(f"the module refused the run: {reason}")
    if "pwgate-not-open" in reason:
        return {"branch": "D", "why": reason, "next": NEXT["D"], **info}
    if info["messages"]:
        return {"branch": "A", "why": f"{info['messages']} mailbox message(s) logged", "next": NEXT["A"], **info}
    return classify_data(out, info)


def main(argv):
    if len(argv) != 2:
        print((__doc__ or "").split("\n\n")[0], file=sys.stderr)
        return 2
    result = score(argv[1])
    print(f"branch {result['branch']}: {result['why']}")
    for key in ("pwgate", "messages", "differing_pages", "post_wait", "reboot_required"):
        if key in result:
            print(f"{key}: {result[key]}")
    print(f"next: {result['next']}")
    return 0 if result["branch"] != "UNSCORED" else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
