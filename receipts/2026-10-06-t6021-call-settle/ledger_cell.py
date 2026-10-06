"""The Coreglass daily ledger's ANE cell on jw14m2, one arm, 3 reps.

Same step, sampler and metric as `coreglass ledger run` (coreglass 071c590:
ledger.measure_linux -> remote.run_cmd with 10 s steps; ledger.capture_metrics
-> "ANE jobs/s"), without the GPU and LLM steps. Arm `after` sets
call_settle_us to 0 inside the step and restores 1000 when the step exits.

usage: python3 ledger_cell.py before|after   (cwd: a coreglass checkout)
"""
import statistics
import sys
from datetime import datetime, timezone

from coreglass import ledger, remote

P = "/sys/module/ane_t6021/parameters/call_settle_us"
arm = sys.argv[1]
host = remote.resolve("jw14m2")
(label, cmd, _), = [s for s in remote.probe_steps(host, {"clusters": []}, 10) if s[0] == "ANE"]
if arm == "after":
    cmd = (f"trap 'echo 1000 | sudo -n tee {P} >/dev/null' EXIT; echo 0 | sudo -n tee {P} >/dev/null; "
           f"[ $(cat {P}) = 0 ] || exit 4; {cmd}")
print(f"step: {cmd}", flush=True)
rates = []
for rep in range(3):
    record = f"captures/settle-{arm}-{datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}.jsonl"
    remote.run_cmd("jw14m2", [], [(label, cmd)], False, 10, None, False, False, 8, 4, 10, record, wait=600)
    metrics, warnings, failed, idle_w = ledger.capture_metrics(record)
    rates.append(metrics.get("ANE jobs/s"))
    print(f"{arm} rep {rep + 1}: ANE jobs/s {rates[-1]} kernel warnings {warnings} failed {failed} "
          f"idle {idle_w:.1f} W record {record}", flush=True)
print(f"{arm}: ANE jobs/s median {statistics.median(rates)} of {rates}")
