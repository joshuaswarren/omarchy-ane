# Regression catalog — ANE/M2 program, last 24 h (2026-09-23 18:43 UTC → 2026-09-24 18:42 UTC)

Source: Main omp session `01a0cb57` (`~/.omp/agent/sessions/-src-ane-linux-experiments/2026-09-22T22-57-55-363Z_*.jsonl`),
all 139 user messages, 252 in-window assistant messages, and all 7 compaction summaries.
Times are UTC. Root-cause classes: **LOST-COMPACTION** (fact was learned but dropped by a
compaction), **POISONED-COMPACTION** (a wrong fact was summarized into a constraint and
propagated), **NEVER-PERSISTED** (fact existed only in conversation), **NO-TOOL** (the
memory/tool needed was absent, so the lesson could not carry), **WRONG-SOURCE** (own
assumption or an unverified report outranked ground truth), **RULE-IGNORED** (a standing
rule covered the case and was not applied).

## 1. Corrections Joshua had to make

| # | When (UTC) | What Main did | Correct fact / behavior | Class |
|---|---|---|---|---|
| R1 | 09-23 20:29–20:33 | Claimed jwm1's macOS needed an interactive login; sent Joshua to log in ("you just wasted my time going to the area where the laptops are and logging in for no reason") | jwm1 macOS answers SSH pre-login at alias `jw-m1-lan` (192.168.3.108); no laptop needs any login (no FileVault, no LUKS). Main admitted at 09-24 11:59: "my false claim caused it, not a real blocker. I tried only two addresses: 192.168.3.66 [its Linux IP] and 100.x" — never the documented alias | WRONG-SOURCE + RULE-IGNORED (`verify-blocker-before-claiming`) |
| R2 | 09-23 20:41–20:51 | Declared the M2 "down / still down" for ~30 min and prescribed a power-button recovery | M2 was up, parked in the m1n1 proxy ("It wasn't down though"). A parked m1n1 proxy is network-invisible, not dead (HANDOFF gotcha already said this) | RULE-IGNORED (never infer "dead" from one failed route) |
| R3 | 09-23 22:49–22:55 | Same power-button demand; in the same status report mixed up the two M1 laptops ("This is the M1 touchbar, Make sure you keep the two M1s separate") | jwm1 = M1 Touch Bar (T8103); jw16/16m1mbp = M1 Max (T6001). The M2 can be rebooted without hands (found 30 min later) | NEVER-PERSISTED (host identity map) |
| R4 | 09-23 22:52–23:24 | Asked for the M2 power button twice before Joshua ordered: "please find a way to reboot it without the power button press" | The m1n1 proxy session on jwm1's ACM can reboot the M2: `p.reboot()` (P_REBOOT). Found in minutes once ordered | NO-TOOL / capability not searched before asking |
| R5 | 09-24 00:01–11:36 | **The overnight false blocker.** Every status update for ~11 h said the M2 ANE was blocked on Joshua's morning login to jwm1's macOS. Joshua 11:36: "Why do I have to login to macos?! ... you have SSH credentials and filevault is off" | Same fact as R1. Once probed at 11:45, the whole recovery (M2 reboot from `jw-m1-lan` via macvdmtool) ran in minutes | WRONG-SOURCE + RULE-IGNORED. Most expensive error of the window |
| R6 | 09-24 11:39 | Wrong IPs; "You messed up and tried the wrong IPs and called a machine down the otehr day" | Use `ssh <alias>` (jwm1, jw14m2-linux, jw-m1-lan, …) — every alias already resolves in ssh config; never hardcode remembered IPs | NEVER-PERSISTED + RULE-IGNORED (`stale-derived-facts` regexes did not match "doesn't answer SSH" shapes) |
| R7 | 09-24 11:55 | Opened GitHub PR #16 against mlx-omarchy instead of merging directly, then abandoned it; had also stayed blocked all night on R5 | Standing order: merge mlx-omarchy changes directly, never leave PRs open (PR #16 later squash-merged d68de5a) | NEVER-PERSISTED (repo convention) |
| R8 | 09-24 15:39 | Asked about M1 Max usage; Joshua: "you own the M1 Max ompletely and wholy for your experiments do wahtever you need to do you don't ask me" | The ownership grant was standing (also in the 15:25 compaction) | LOST-COMPACTION |
| R9 | 09-24 17:16–17:41 | Asked Joshua to power-cycle the M2 a third time and skipped the webcam check. Joshua: "Did you use the webcam ... I'll go power cycle it even though you forgot this step." Main admitted 17:41: "I called the M2 dead because it wasn't answering on the network. That was wrong." | Before "dead": grab a webcam frame; before power button: try `p.reboot()` over the USB proxy | LOST-COMPACTION (p.reboot absent from the live summary) + RULE-IGNORED |
| R10 | 09-24 17:50–17:54 | Asked Joshua to "type the M1's disk password". Joshua: "WTF - There's no disk password, and you KNOW there's no disk password. The title of this window ... is even 'remember laptops lack disk password'" | No disk password/encryption exists on any laptop. The "password screen" was a misread webcam frame (reflection). The fact was in the session title, the goal text, and five compactions — and was still dropped the moment a blurry frame suggested otherwise | POISONED-COMPACTION (frame) + WRONG-SOURCE |
| R11 | 09-24 17:55 | "And you are probbaly checking the wrong IP AGAIN" | See R6 | NEVER-PERSISTED |
| R12 | 09-24 17:59–18:01 | Claimed M1 showed a login screen, then claimed "Don't power-cycle the M1. It's running ... up 51 minutes" while Joshua, standing at it, saw a hard crash (black screen). Joshua power-cycled it himself ("m1 is on 3.66 ... it's up and running now") and had to order no-sleep setup ("make sure you have all these laptops set not to sleep so that this doesn't happen again") | A machine state claim needs a frame + an ssh identity cross-check, not one stale probe. Laptops sleep/crash unless no-sleep is applied (applied 18:03 on jwm1 + jw16; M2 pending) | WRONG-SOURCE + NEVER-PERSISTED (no-sleep duty) |
| R13 | 09-24 18:14–18:18 | Started fresh prior-art research without checking its own repos: briefed the scout to study eiln/ane as outside prior art. Joshua: "did you forget about omarchy-ane?", "eiln/ane never moved past the M1", "that's the driver you're supposed to be building M2 support into" | omarchy-ane IS our fork of eiln/ane (stated in omarchy-ane/AGENTS.md); eiln never went past M1; no public M2 ANE work exists. Check own repos/receipts before commissioning research | LOST-COMPACTION + NEVER-PERSISTED (coordinator cwd ≠ omarchy-ane, so its AGENTS.md never loaded) |
| R14 | 09-24 18:24–18:31 | Presented M1/M1 Max firmware-start as an open question and began "porting" M2 startup to the M1 Max. Joshua: "Right, but you start the ANE firmware successfully under the M1 (non-Max)" | omarchy-ane runs the ANE fully on M1/M1 Max: the host drives TM/TQ directly (`ane_tm_enable`) with no Linux-side firmware boot (iBoot preloads it). T6021 is the chip that needs an ASC firmware start. The morning receipt `local://t8103-fwboot-diff.md` already said this | LOST-COMPACTION |
| R15 | 09-24 18:28–18:41 | After the M2 wedged again: asked for the power button again, and excused the unused camera with "that camera hung jwm1 earlier". Joshua: "You went through all of this about 12 hours ago. Did you not learn anything from it? Have you been using Remnic like you're supposed to?" ... "You were using the camera just fine yesterday." The 18:41 grab (`timeout -s KILL 25 sudo ffmpeg ... -i /dev/video1 ... -frames:v 40 -update 1 /tmp/m2now.jpg`) worked first try | jwm1 `/dev/video1` watching the M2 works (it worked all day 09-23). The "hang" was one unguarded ffmpeg plus jwm1's own 12:42–12:59 network drop — and the wrong conclusion had been written INTO the 18:22 compaction as a constraint ("do NOT use it"), poisoning later context. Remnic was not in use (no tools mounted, no CLI) | POISONED-COMPACTION + NO-TOOL |
| R16 | 09-24 18:41 | "Why do you keep regressing?!" — the meta-correction | See the class tally in §3 | — |

Pre-window corrections carried by summaries (same classes repeating; kept brief):
09-22 23:06 (no LUKS/FileVault anywhere; use Remnic; MUST run parallel subagents) ·
09-22 23:16 (first webcam-reflection misread, M2 was in macOS recovery) ·
09-22 23:23–24 (doubted the camera map; told agents the M2 was invisible from jwm1) ·
09-23 13:11 ("please stop breaking the laptops so much ... A week or so ago you never broke the boot") ·
09-23 13:43 + 14:43 (asked Joshua to log into the M2 — it answers SSH at boot) ·
09-23 14:02 ("Use Remnic please") ·
09-23 15:15 ("Are you sure ...? Make sure you're not making assumptions").

## 2. Errors Joshua did not catch

| # | When (UTC) | What happened | Correct behavior | Class |
|---|---|---|---|---|
| U1 | 09-23 22:12 and 09-24 13:44→18:28 | ANE registers read while the ANE was unpowered — twice. First read froze the M2 (self-reset); second wedged it and forced a manual power cycle. Standing rule already said: never read the engine window unpowered | Gate every engine-window read on pmgr islands reading ACTUAL=0xf (guard committed only after the second wedge: `agent/m2-ane-obs` ba9a7d0) | RULE-IGNORED |
| U2 | 09-24 15:52 | The NO-PGD firmware-mapping fault (IOVA 0x100000dca10) from 2026-09-20 was re-created by the B2 DART probe reset — a known fault class from four days earlier re-triggered because the lesson lived only in a receipt nobody re-read | Re-check receipts for the exact fault signature before re-running a similar sequence | NEVER-PERSISTED |
| U3 | 09-24 16:33–16:39 | Ran an M2 firmware-start test whose mode-setting write "recorded nothing, so we can't prove the setting was applied" — "The last test doesn't count either way." A full M2 boot cycle spent on an unverifiable run | Build the log/readback into the test before spending a boot cycle (receipts-first at design time) | RULE-IGNORED |
| U4 | 09-24 08:46–09:23 | Contaminated A/B on jw16: an orphan nohup benchmark from CopyCast held the GPU lock and left llm-inference stuck "activating"; earlier tests had also enabled the shelved fused kernel, so 21 "flips" were venv contamination, not kernel behavior | Agents kill their own background jobs; measurement windows verify a clean environment (fresh venv, lock holder, service state) before attributing flips | RULE-IGNORED |
| U5 | 09-24 12:09 | Jwm1Decode signed off while still holding jwm1 during the M2 trace window | One owner per laptop; handoff must be explicit | NEVER-PERSISTED |
| U6 | systemic | Compaction drift: the p.reboot() path (learned 09-23 23:24) is absent from every later compaction's constraints, while the WRONG camera ban was promoted into the 18:22 compaction. jw16 alias flip-flops (`16m1mbp` vs `jw16mbp1-linux` — both resolve to 192.168.10.244) | Durable facts belong in a loaded rule/memory, not in summaries; wrong facts must be corrected at the source, not just in chat | LOST-COMPACTION + POISONED-COMPACTION |
| U7 | 09-22 23:16 (carried forward) | A KNOWN-WRONG camera fact (`fact-1790118755410-jzx7`) was stored in Remnic, and its supersede was still blocked on the router days later — the canonical memory store held a live wrong fact | Supersede wrong memories immediately or delete them; do not leave a blocked correction behind | WRONG-SOURCE |

## 3. Class tally (window errors)

WRONG-SOURCE: R1, R5, R10, R12 (+U7) · RULE-IGNORED: R1, R2, R6, R9, U1, U3, U4 ·
LOST-COMPACTION: R8, R9, R13, R14, U2, U6 · POISONED-COMPACTION: R10, R15, U6 ·
NEVER-PERSISTED: R3, R6, R7, R11, R12, U5 · NO-TOOL: R4, R15.

One meta-pattern: every class feeds the others. A fact that is never persisted
(R6) is lost by compaction (U6), replaced by a plausible guess (WRONG-SOURCE),
and the standing rule that should have caught it (verify-before-blocker) is
left as the only line of defense — which then also fails when the rule's
trigger never fires. The fix set in the companion rule `ane-fleet-facts` and
`ane-fleet-verify-before-human` attacks each link: canonical facts loaded at
mention-time, and a hard ladder (probe → documented path → webcam + ssh
identity check → only then ask Joshua) loaded at mistake-shape time.
