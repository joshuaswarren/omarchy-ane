# Changelog

## Unreleased

- `promotion_check`: do not judge a block whose check output binds both dies. This covers runtime and fallback rows in either line order, even with a scalar die field. Rows that bind one die keep their old behavior.
- T6020 (M2 Pro) ANE back to opt-in. (row 3c9389040f51,02ba4b462a7d; dkms,intree)
- The H13 driver attempts recovery before clearing a wedged session. Failed recovery retains the wedge, module pin and preserved buffer mappings. CPU state tests cover close, recovery failures and successful release; hardware retirement is not qualified by these tests.

- `ane_h15`: add the off-by-default T8122 runtime overlay from its own J613/T8122 DT evidence. Keep DARTs disabled and omit `iommus`; correct ANE_BASE to depend on both ANE_TD and ANE_CPU. Offline checks do not establish hardware execution.

- `ane_h15`: stage 2 can compare the current domain's RVBAR entry with TEXT from the same boot's ADT. It refuses a missing domain, changed RVBAR or zero translation and fails a different address. It creates no mapping and starts no core; a match proves only that entry.
- tools: the M3 part 2 scorer isolates each run's messages and gate state. It rejects malformed page indices and comparison records. An empty capture no longer implies a missing device-tree node.

- docs: the README and the level 5 receipt now give the Qwen decode at levels 3 and 4 and a 31 minute decode soak at level 5: the S1-GPTQ step reaches its floor at level 3 (0.0790 s, then 0.0788 and 0.0787 s), the fp16 step does not follow the clock, and the logits are identical at every level.

## 0.5.7 (2026-10-30)

- T6022 (M2 Ultra) ANE on by default. (row 316ea37e17f0; dkms; die 0 only; kernel 7.1.13 with the ANE DART power-domain change (#278))
- ane: on the 16-inch M1 Max (T6001) the driver now asks for ANE clock level 5, the top of the power manager's six levels, at power-up instead of level 1. The Parakeet encoder takes 199.0 ms instead of 289.7 ms, bit-exact; a 30 minute soak of 9,000 calls held every block within 0.05 percent and 49 C at the end. The Qwen decode step with the S1-GPTQ packages falls from 0.1010 s to 0.0787 s; with the fp16 packages it does not change (0.1200 s to 0.1184 s). The module parameter `ane.dvfs_level` opts down (`1` to `4`, or `0` for no request). Only the T6001 overlays carry the clock windows, so the M1, M1 Pro, M2 Pro and M2 Max are unchanged. Receipt: `receipts/2026-10-30-ane-level5-soak`.
- docs: `docs/parakeet-encoder-pairs.md` is new. It puts the whole Parakeet encoder on Linux next to the same cell on macOS, on the same Mac: the 13-inch M1 at 80.5 percent of the macOS throughput and the 16-inch M1 Max at 47.4 percent, with what the numbers do and do not show.
- docs: `docs/qwen-decode-packages.md` gets the 16-inch M1 Max rows at 1,024 positions on one boot: clock level 0 (22.0 percent lower with the 8-bit package) and clock level 1 (15.7 percent lower), with the level 0 to level 1 factors (1.73 and 1.60).
- docs: `docs/qwen-decode-packages.md` gets the 13-inch M1 rows at 1,024 positions (two sessions, 28.3 and 28.1 percent lower) and a third 2,048-position session (13.4 percent lower), with the two 1,024-position package hashes.
- docs: `ane/h15/README-bringup.md` gets two stop rules. On a 16-inch M3 Max (T6031) the machine went down during a CPU read of the firmware DATA on two boots (on the second, which logged each page, at page 232), so hash and dump scripts stop at page 231. A read-only probe for a volunteer's machine first reads only the ranges the driver itself uses, and a crash stops the run for a fix. The PWGATE run instructions on issue 176 were changed to match. The module itself never reads those pages.

## 0.5.6 (2026-10-29)

This release changes little for users of the M1 and M2 families. The M3 bring-up module renames two parameters, and the base M2 (T8112) opt-in module gets one safety fix. One overlay fix lets the M2 Ultra (T6022) boot. Nothing in it makes the ANE run on an M3, and default settings behave as in 0.5.5.

Credits: Ophuscado found and fixed the M2 Ultra boot panic (PR 278) and gave the first T6022 report. invaliddayta ran the first base M2 volunteer run, and a review of that run found the gap behind the PWGATE latch fix.

- data: the key names in `data/ane-soc/*.json` are neutral now; the test fixture `t9999.json` renames one key; the check test gains a case for the download host alone; the M4 receipt drops a hash row that pointed at a file not in the tree.
- docs: the receipts and header comments state hardware facts only. The `data/ane-soc/*.json` records lose their driver sections and image fields, and their source kinds are now `hw-adt`, `hw-behaviour`, `hw-chip-map` and `hw-firmware`, which name the hardware fact and not where it came from. `omarchy-ane-firmware-fetch` keeps its download addresses in code, and its header is a usage paragraph.
- tools: `tools/check_no_macos_read_methods.py` and the `public-text` workflow stop new public text that shows how we read macOS: kernel addresses, code offsets, extraction or decode commands, restore-image locators, Apple symbol names and decode wording. It is a ratchet with a baseline of 1 file (1 line, our own vmlinux disassembly). The baseline must equal the tree, so a cleaned file cannot regain matches until someone lowers its entry. The patterns are literal, so they miss plain words. The test builds a throwaway repository and covers 60 cases.
- `ane_h15`: the stage 5 options `kext_boot` and `kb_wait_ms` are renamed `scratch_boot` and `sb_wait_ms`; the RESULT text and the log lines follow (`scratch_boot=1 s7_ready=...`). The old parameter names are still accepted at module load, hidden from the parameter list, until the commands already posted to volunteers have run. Behaviour is unchanged. The sim passes with the new names.
- docs: `ane/h15/README-bringup.md` now says that the `pwgate_t6031` register facts are macOS behaviour that we have not measured on an M3, and that the full notes stay private. It also says what they do not show: why the step applies to a T6031 only, and that naming the fifth ADT window PWGATE is an inference. This replaces the draft receipt of PR 275.
- `t6022-ane` overlay (M2 Ultra, opt-in): the three ANE DART nodes now take their power from the ANE_SYS state (pmgr `0x260`) instead of pmp and ANE_CPU. On a J475d the shipped overlay panicked the boot with an asynchronous SError in the DART probe on most boots (kernel 7.1.13). With the change the machine boots, the DARTs initialize, and the add smoke fixture ran 20 of 20 calls bit-exact. This is the first T6022 boot report. We have no T6022 machine; the panic and the run are the reporter's (Ophuscado, PR 278). The `ANE-SYS-DART` gate is on `dart-ane0` in the device trees of the M2 Pro and Max; that the gate sits under ANE_SYS comes from the reporter's IORegistry reading. The overlay builds against the j180d and j475d device trees in CI. T6020 and T6021 keep the shared values; whether they need the same change is not known.
- `ane_t6021` (T8112, `pwgate_open=1`): a poll timeout after the one PWGATE write now latches the module as dirty. A review of the first M2 volunteer run pointed out the gap. Before this change the probe returned `-ENODEV` with the latch clear, so a later probe in the same boot (a deferred probe, or a manual bind) could read the word again and write a second time. Now that probe is refused with "reboot required", so a timeout is never followed by a second write in the same boot. A write that works leaves the latch alone, so a later clean failure of the same probe, such as the `fw_start=0` defer, stays retryable without a reboot. That retry powers the ANE domain up again, and if the gate word then reads `0x30000000` again (not measured), `pwgate_open=1` writes a second time at every power-up. A new check, `tools/test_ane_pwgate_latch.py`, reads the driver source and fails if the latch is missing, comes before the poll, or is also set on success. It does not run the driver, and nothing was run on a T8112 for this change.
- docs: `docs/qwen-decode-packages.md` now says that every 13-inch M1 and 14-inch M2 Max step time in it was measured with the ANE clock set by the boot. The driver has no clock control on those chips yet, so their level is unknown.
- docs: the two 16-inch M1 Max rows at 2,048 positions in `docs/qwen-decode-packages.md` were measured on boots without the PMP firmware, so the ANE ran at clock level 0. They are relabelled, and two rows with the PMP running are added (step 0.1253 s for fp16, 8.3 percent lower for the proposed package). The third 14-inch M2 Max session on another boot reads 9.50 percent lower.
- `ane_h15` stage 5 gains `pwgate_t6031` (default 0, T6031 rows only). On a T6031 a gate word has to clear before the core is released: the step clears the gate word of the
  PWGATE window and polls bits 1:0 until they read 0 before it releases the core. That
  window is the fifth reg entry of the ADT ane0 node, size 0x4000, next to the pmgr window. With `pwgate_t6031=1` the module reads
  and logs that word as the last action before the first CPU_CONTROL write; if bits 1:0 are
  set it writes 0 once and polls up to 5000 times, 10 microseconds apart, and an already
  open gate is logged and left alone. `pwgate_t6031=2` always writes 0 first, the
  unconditional macOS order. Other values, other stages, and the rows without the window
  refuse in probe before any bus access and before any mapping. A poll timeout stops the
  run with `FAIL pwgate-not-open` before any CPU_CONTROL write: the core never starts,
  nothing is pinned, and every window is unmapped again. A successful run adds a
  `pwgate=` suffix to the RESULT reason. The two T6031 stage 5 runs ended with the core at
  CPU_STATUS `0x30002a` to `0x300028` and no mailbox message; the hypothesis under test
  (an inference, not measured) is that a gate still closed there idles the
  core. macOS behaviour, never run on an M3. The sim pins all of it: one write
  at most, the T6031-only refusal, the timeout stop, and the byte-identical default.

## 0.5.5 (2026-10-28)

This release is mostly M3 bring-up, and nothing about it makes the ANE run on an M3. Users of the M1 family and the M2 family get no change: the smoke program, the Qwen packages and the modules behave as in 0.5.4 with default settings. One new module parameter, `pwgate_open`, is off by default and applies only to the base M2 (T8112), where it has never run. On the M3 family (T8122, T6030, T6031, T6034) nothing is supported and no job runs. The H15 module stays an opt-in bring-up tool. What is new is evidence and tools. On a 16-inch M3 Max (T6031) stages 0, 1, 2 and 4 passed, all read only. RVBAR reads 0x10000000001 and the firmware ranges were the same on two boots. The stub firmware matches the ANE firmware installed on that board on 41 of 44 TEXT pages. On a 16-inch M3 Pro (T6030), stage 5 ran earlier and left the core idle with no mailbox message. The module gains an option that starts the core in a fixed boot order (`scratch_boot=1`), a check that stops that option when the SCRATCH7 clear does not read back as zero, and an interrupt acknowledge in stage 7. None of those three has run on an M3. Still unproven on every M3: that the ANE core runs firmware, any mailbox message, any job, a boot with the T6031 DART overlay, and the smoke program, which exits 2 on every M3 chip.

### M3 (H15): module, overlays and volunteer results (experimental)

- `ane_h15` stage 5 gains `scratch_boot=1` (off by default) with `sb_wait_ms` (10 to 5000, default 1000). Instead of the read-modify-write it starts the core in the firmware's own boot order. That order has never run on an M3. It writes 0 to all eight SCRATCH words, then SCRATCH6=1, then SCRATCH7=0 with a read back, then CPU_CONTROL 0 and 0x10. Then it polls SCRATCH7 every 1 ms for the ready word `0x08042006`. The T6030 stage 5 runs ended with the core at CPU_STATUS `0x300028` (IDLE) and no mailbox message. The hypothesis under test (an inference, not measured) is that a stale ready word in SCRATCH7 sends the firmware down the warm-boot path, where it idles. The option is refused on every stage other than 5, before any bus access. It never writes RVBAR, the mailbox or the DART, and SCRATCH5 receives 0 only (the config value there is unknown). The RESULT line carries `scratch_boot=1 s7_ready=<yes|no>`, so a volunteer log decides the hypothesis. The verdict rules are unchanged. `sim-test` pins the default sequence byte for byte, the exact scratch_boot sequence, both RESULT reasons, the stage 2 and 4 refusals and the `sb_wait_ms` gate.

- `ane_h15`: `scratch_boot=1` now stops before RUN when the SCRATCH7 clear does not take. An outside review found that the read back after the SCRATCH7=0 write was only logged. A register that ignores the clear keeps reading the stale ready word `0x08042006`, and the poll then accepted it as `s7_ready=yes` while the firmware never ran. The read back must read 0 now; anything else stops the run before the first CPU_CONTROL write and the stage fails with `scratch_boot=1 s7_ready=no clear-failed`. The core never started in that case, so nothing is pinned and no reboot is needed. The write order is unchanged. `sim-test` models a SCRATCH7 that ignores the clear and fails the old behavior on purpose.

- `ane_h15` stage 7 services the firmware interrupt pair that sits next to the SCRATCH block. In its stage 7 wait loops (the table wait, the SCRATCH3 wait, the ring answers) the module reads the pending word at engine + 0x184c000. When it reads a non-zero value it writes that value once to the acknowledge register at engine + 0x1850000, after a log line with the value and the address, and it logs the pending value whenever it changes. At most 64 acknowledges happen per run. A zero pending writes nothing. The pending register is never written. Stages 0 to 6 are byte-identical to before: they never read the pending word and never write the acknowledge. The pair matches the one the M6 module already services at engine + 0x18ac000 and + 0x18b0000, where the firmware waits for an acknowledge. The sim harness pins all of it: pending 0 gives no write, a raised pending gives one write of the value read, the cap holds, stage 6 stays untouched. Never run on an M3.

- `ane/h15`: the T6031 row now carries the measured RVBAR `0x0000010000000001`, from stage 2 runs on a 16-inch M3 Max reported on issue 176. Two boots read the same word the T6030 row carries. Stage 5 passes the RVBAR gate on a T6031 now; t8122 and t6034 still refuse it. The sim pins the new truth: the old t6031 refusal check became a full stage 5 run check. A T6031 `map_fw=1` run uses the T6031 DART overlay, which has not been booted.

- `ane/h15`: a T6031 DART overlay joins the T6030 one, `t6031-ane-dart-experimental.dts`. It enables the three ANE DART translation instances (windows 0, 1 and 2), lists the `ane` node as a client of all three with stream 0, and leaves the DAPF aperture window at 0x3c9804000 disabled. It is for `ane_h15` stage 5 with `map_fw=1` on a T6031, and only after stage 4 finds window 0 reading as a DART there. Stage 4 on a J516C did, with no stream enabled. The file has not been booted; the T6030 model booted on a J516S with one and with three instances without a hang, and an enabled DART node without power domains hung a T6030 at boot before. It compiles with dtc and gives the same four warning classes as the shipped T6031 overlay.

- `ane/h15`: `h15-segments.py` finds the ANE node named `ane0` first, then `ane`. A J516C names the node `/arm-io/ane0`, and the script refused its ADT dump with `no /arm-io/ane node in this ADT`. Every other refusal is unchanged. A new host test (`tests/test_h15_segments.py`) builds a synthetic ADT in memory and pins the three cases: an `ane0` node, an `ane` node, and no ANE node.

- `omarchy-ane-smoke`: the M3 chips (T8122, T6030, T6031, T6034) now find an add program, `fixtures/h15-hwx/add`, compiled for the h15 target on a Mac and never run. They exit 2 with `no driver for this chip can run a job yet` before any module load or job. M1 and M2 behaviour is unchanged: a reviewer compared 18 fake-root cases against the base, and all matched. The README of the fixture says plainly that it has never run on an M3.

- docs: the README and the two T6030 and T6031 DART overlay headers said the T6030 three-instance overlay had not been booted and that stage 5 and `map_fw=1` had never run on an H15 part. Both did run on a 16-inch M3 Pro, with one and with three DART instances, and both ended in `core-not-stopped` with no mailbox message. The text now says so. It also says that the T6031 overlay, the cold-boot option and stage 7 have never run on an M3.

### Base M2 (T8112): module (experimental)

- `ane_t6021`: a module parameter `pwgate_open` (default off) lets the T8112 probe open a closed ANE power gate. When the gate word reads exactly 0x30000000 it writes 0 once and waits up to 100 ms for bits 29:28 to clear. Any other word refuses without a write. The decision is a small header shared with a host test, `tools/test_ane_pwgate.c`, which CI runs. It has not run on base M2 silicon.

## 0.5.4 (2026-10-28)

This release adds a script that loads the PMP firmware when ANE work starts, so the PMP can stay out of boot. On the 16-inch M1 Max, loading the PMP at every boot made GPU decode about 10 percent slower (the boot without it decodes 11.5 percent faster). Six fresh boots in the order without, with, with, without, without, with gave Qwen3.8-2B 4-bit decode with mlx-lm at 92.7 against 83.1, 92.8 against 83.2 and 92.4 against 83.1 tokens per second, a median pair gain of 11.54 percent for the boot without the PMP, against a bar of 5 percent. The ANE clock level still needs the PMP; ANE work can load it first with `tools/ane-pmp-load.sh`. The release also names the 8-bit Qwen decode package as the one to run on the 13-inch M1 and the 14-inch M2 Max (fp16 stays the opt-out; the repo has no switch for the package), and fixes a refused firmware command on the experimental M6 (T8152) path.

- `tools/ane-pmp-load.sh` starts the PMP firmware on demand for the ANE clock level on the 16-inch M1 Max, and the README says the module cannot be unloaded. It waits for the PMP device to bind or for the ready line, and exits 4 if the module is missing, fails to load or is not ready after 90 seconds. Its test covers five cases with stub `insmod`, `dmesg` and `sleep`.
- docs: the 14-inch M2 Max 8-bit package is about 9.5 percent faster than fp16 at 2,048 positions. Two sessions on one boot read 9.76 and 9.45 percent, both under the 10 percent bar, with no GPU job running. The page now says so. The earlier 10.6 percent was one session at 192 positions. The runs are in `receipts/2026-10-27-m2-l2048-speed`.
- docs: the 14-inch M2 Max has a 2,048 position quality result for the 8-bit default package. On 12 prompts the KL ratio is 1.13 against the limit of 2. Each prompt has one window of 24 positions. The page lists the limits. The sample is thin. The int8 weights are calibrated at 192 positions. There is no 1,024 position check.

- docs: the 14-inch M2 Max joins the 13-inch M1 on the 8-bit default Qwen decode package, with fp16 as the opt-out. The M2 Max met the rule fixed before the runs: on 161 unseen long-position prompts the KL ratio is 1.31 and the top-1 gap is +0.0006 against a limit of 0.001. The top-1 margin is thin, the interval reaches +0.0015, and there is no longer-context check on the M2 Max.

- docs: `docs/qwen-decode-packages.md` sets the long-position selective int8 package as the default Qwen decode package on the 13-inch M1, with the fp16 package as the opt-out. It gives the evidence with every limit: the top-1 gap is not resolved, quality ran on the 13-inch M1 only, and two speed runs missed the 1 percent agreement rule. It has a rollback line. Docs only; the repo has no switch for the package.

- `ane/h18`: a refused MCACHE_SIZE_GET (`hs_mcsz`) is now logged and the run goes on, like the other optional commands. Before, the refusal stopped the step with an I/O error. The module pin in the stage script changes with it.



## 0.5.3 (2026-10-25)

This release fixes a load-order refusal of the shipped Qwen package on the M2 Max. It makes a selective int8 Qwen package the opt-in speed tier. It also carries more experimental M6 (T8152) program-stage work. On the M2 Max the 38-program package refused to load after other sessions on the same boot. Loading the largest programs first loaded all 38 in eight sessions in a row. That was a load-only test. A full decode run after the change, and other packages, are not shown. The selective int8 package keeps the five attention programs in fp16 and the ten MLP programs in int8. It cuts the step by 14.5 percent on the 13-inch M1, 11.3 percent on the 14-inch M2 Max and 8.7 percent on the 16-inch M1 Max. It passes the technical gate and the code rule. On long-position prompts its KL is 1.54 times the fp16 KL, and its top-1 misses the limit on both chips it was scored on. So the fp16 package stays the default. On the M6, an H18-compiled add program, and an add followed by a multiply, loaded, started and finished with the right answer. An earlier add run gave 4.5 where 3.75 was expected, from a guessed buffer tag table. Matmul programs, which have weights, loaded and started but never finished. The new weights probes have not all been run. A new `hs_pmu` option sends a firmware command the module left out (SET_SNE_PMU_BASE2). The firmware accepted it, and the matmul still did not finish. A new `hs_dyn` option sends the dynamic power gate command; it is untested. No default changes on any chip.

### M6 (T8152): module, device tree and program pins (experimental)

- `ane/h18`: two module parameters send more of what macOS sends. `hs_defset` sends DEFAULT_SETTING_SET (0x2e, one setting, register id 2, value 10000) after the five property writes. macOS sends this on this chip and the module never did. The firmware handler skips register id 2, and what consumes it is not known. `hs_mcsz` sends MCACHE_SIZE_GET (0x26), a read only command, after CONFIG_GET and logs the size from the reply. A size of 0 would point at the memory cache quota that macOS requests from the SoC and that Linux does not request. The M6 matmul still stalls after the PMU base, power gating and neural engine count changes, and these are the remaining commands of macOS's start sequence. The stage script takes two more arguments, `defset` and `mcsz`, and pins the rebuilt module.

- `ane/h18`: five program files join the pin list of the stage script. Each is an earlier probe program with one change: the number of neural engine cores in the operation section is 16 instead of 0. The macOS ANE driver writes that number, taken from the program's thread state, into every operation record. The Linux packer left it at 0 for all programs. An add does not need the neural engine cores and finishes. Every matmul stalled. The files are the 128, 512 and 2048 matmuls with the weights in the kernel section, the 128 matmul with the weights as their own buffer, and the add as a check that it still finishes. Whether the firmware uses the number is not known. Nothing is run yet.

- `ane/h18`: a module parameter `hs_dyn` sends SET_DYNAMIC_POWERGATE (0x2d) right after the PMU base command: 1 turns the firmware's dynamic power gating off, 2 turns it on. Default off. macOS builds this command, but where it sends it is not known. The firmware needs the PMU base first, so the module sends it after that. On the M6 the PMU base command was accepted and the matmul still did not finish, so power gating is the one power setting left that the module has not tried. It may change nothing. The stage script takes a third new argument, `dyn`, and pins the rebuilt module.

- `ane/h18`: a module parameter `hs_pmu` sends a firmware command the Linux module left out, SET_SNE_PMU_BASE2 (0x29), after START and before CONFIG_GET, with the value 0x300700000. The firmware uses it as the base of its own power control registers. No other per engine power step is known. The matmul programs that stall on the M6 use the neural engine cores, and the adds that finish may not. If those cores are never powered, the missing command would explain it. That is a guess until a board run. The stage script takes two new arguments, `pmu` and `pwait` (a program wait of 5000 ms instead of 1000), and pins the new module.

- `ane/h18`: three more program files join the pin list of the stage script: matmuls of 128 x 128, 512 x 512 and 2048 x 2048 whose weights are their own call buffer. The earlier matmuls folded the weights into the kernel section. The macOS ANE driver does something else with a `__KERN` segment: it maps the segment as a separate buffer and keeps it out of the kernel section. In the earlier files the weights slot may have pointed at the base of the kernel section instead of at the weights. On the M6 those matmuls loaded and started but never finished, while an add and an add followed by a multiply, which have no weights, finished with the right answer. The new files tell whether the weights path was the cause. Nothing here is run yet.

- `ane/h18`: the stage script pins a list of program files, not one. Any file in the list may be staged for a board run, so one review covers several programs. The list holds the 2048 x 2048 matmul that stalled on the M6 and three probes built to find out why: an add followed by a multiply (two tasks, no weights), and matmuls of 128 x 128 and 512 x 512 (weights in the kernel segment). On the M6 the matmul loaded and started, then never finished. The firmware posted the start events but not the finish event, and the DART showed no exception. The probes change one thing at a time: the number of tasks, or the size of the weights. Which program finishes tells which one the firmware dislikes.

- `ane/h18`: the stage script pins an H18 matmul program, the first with weights. It is a 2048 x 2048 fp16 linear layer (a Qwen decode shape) compiled for the H18 engine with constant weights of 0.25 and an input of ones. Every output element must therefore be 2048 x 0.25 = 512, which is 0x6000 in fp16. The H18 compiler puts the weights in a separate 8 MiB segment named KERN_0, not in the text constants. The program file takes them as the constants of the kernel section. Its operation section gets the BAR slots from the flavor-4 thread state of the HWX (slot = qword index >> 1), a fixed rule (the loader that builds the slot to buffer pairs), not a guess. Whether the firmware accepts a kernel segment this way is untested.

- `ane/h18`: the first program ran on the M6 ANE. The add program compiled for the H18 engine loaded, started and finished: the call took 1 ms and the firmware posted three events, the last with state 1. The engine wrote the output buffer and the DART showed no exception. The H14 add program had stalled in the same sequence and the H18 program finishes, so the stall is a property of the H14 program file, not of the loader, the DART or the firmware settings. Which part of the file matters (instruction set, descriptor or task stream) is not known. The output was 4.5 in every element, not the expected 3.75. The guessed tag table gave both inputs the same buffer, so the second input was never read. The pin now names a program file with the corrected table, which is still a guess. The rule macOS uses is not used yet.

- `ane/h18`: the stage script pins a program file made from a program compiled for the H18 engine. The Apple compiler on a Mac Studio accepts the target `h18` (Mach-O cpu subtype 10). It emits the same add program as for H14: two 128 x 128 fp16 inputs and one output. The task stream is longer, 316 bytes instead of 260. The program descriptor is 0x10 bytes longer, and its buffer IOVA array starts 0x20 bytes later. The program file was packed from an ANEC that a two line variant of `hwx_h14_staged_to_anec.py` converted. A local change of the section builder assigns the buffer tags by BAR slot for the H18 register words it does not know. That tag table is a guess. The H14 add program ran on the M6 until the firmware acknowledged the load, the process and the call, and then it never finished. No host side setting changed that. Whether the firmware finishes an H18 program is the question of the next board run.

- `ane/h18`: `hs_props` takes 0, 1 or 2 (stage script argument 14 likewise). The first M6 run with `hs_props=1` showed the firmware acknowledges all five property writes (ack 0) and still never finishes the first program, with the console silent after START PROCESSED. `hs_props=2` sends property 0xa1 = 1 instead of 0, the property macOS writes as 0, which may make the firmware log what it does with a call. Not tested on a board: whether 1 changes anything. A value above 2 is refused before any write.

### Packages, docs and tools

- A selective int8 Qwen decode package replaces the mixed package as the opt-in speed tier in `docs/qwen-decode-packages.md` (seals 3fbb7a30 for the M1 and d628ebc4 for the M2 Max). It keeps the five attention programs in fp16 and the ten MLP programs in int8, with the mixed GPTQ calibration. On the 13-inch M1 and the 14-inch M2 Max it passes the technical gate and the code rule, and its long-position KL is 1.54 times the fp16 KL on both chips. It misses the top-1 limit of 0.001 on long positions on both chips (gaps of 0.0024 and 0.0015). The fp16 package stays the default. The step is 14.5 percent lower on the 13-inch M1, 11.3 percent on the 14-inch M2 Max and 8.7 percent on the 16-inch M1 Max, against 29.4, 21.3 and 16.6 percent for the first GPTQ package.

- `tools/qwen_m2_decode.py` loads the programs of a package largest first. On the M2 Max the shipped package (38 programs, 2.8 GB of sections) refused to load after other sessions had run on the same boot: `device-open-failed` at a large section, from `DRM_IOCTL_ANE_BO_INIT` failing with ENOMEM. A load-only test on one boot showed the cause is the order, not a leak. The pool was empty and `bo_total_bytes` was 0. Index order refused the 38th program and reverse order refused the 7th. Largest first loaded all 38 in eight sessions in a row, across sessions of both packages. The sections come from the IOVA allocator aligned to a power of two inside a window of about 3 GB, so the order decides whether the last large one fits. A refused session leaves nothing behind: `bo_total_bytes` went back to the parked pool when its process exited. The earlier reading of a leak came from counters read while the process was still shutting down. Not shown yet: a full decode run after the change, and other packages than the two tested.

- A second int8 GPTQ package calibrated on prose, multilingual and code prompts is listed in `docs/qwen-decode-packages.md` (seals 829b43ab for the M1 and 539b3c73 for the M2 Max). On a 13-inch M1 it keeps the technical-set KL (0.000390 against 0.000397) and cuts the KL on 26 code prompts it never saw from 1.60 to 1.38 times the fp16 KL, which passes the rule the first package failed. On the 170-position prompt set it is worse (2.26 times against 2.06). Both packages stay opt-in speed tiers. The M2 Max package has not been run.

## 0.5.2 (2026-10-25)

This release raises the M1 Max ANE clock to level 1 in the driver, adds the int8 GPTQ Qwen decode package as an opt-in speed tier, and carries the M6 program-stage work: DART instances 0 to 3 with their tunables, the firmware console log, PREMAP_BUFFER and a first program load. It gives the T6030 overlay all three DART banks and the H15 module a read-only SCRATCH dump, and it moves the promotion release step onto one release per run. On the 16-inch M1 Max a Qwen decode step goes from 0.2009 s to 0.1158 s (4.98 to 8.64 tokens per second) with bit-identical logits: one machine and five prompts. A one-hour suspend and resume soak on the same machine then ran clean: 1101 of 1101 level-1 resumes, logits bit-identical in 7 decodes, no bad kernel log lines, 44.6 C at most. The receipt in the repository still lists the soak as not done. The GPTQ package cuts the step by 21 to 29 percent and passes the quality gate on technical text on one M1 and one M2 Max, but it fails the rule on an M2 Max's code and long-position prompts (KL 1.57 and 2.06 times the shipped package). On the M6 no program has finished yet, and the M3 Pro stage 5 runs showed CPU_STATUS with STOPPED clear and IDLE set, and no mailbox message. No default changes on any chip.

### M1 Max (T6001): ANE clock level

- ANE clock level on the T6001 (M1 Max): ane.ko requests level 1 from the PMP
  when the partition comes up and releases it before the partition goes down
  (`ane/src/ane_dvfs.c`, runtime resume and suspend), in the order macOS uses.
  A full Qwen decode step on the 16-inch drops from 0.2009 s to 0.1158 s (4.98
  to 8.64 tokens per second) with bit-identical logits; level 2 adds under 3
  percent and higher levels were not written. The driver needs two more `reg`
  entries and the PMS SRAM power domain on the ANE node (only the T6001 overlay
  carries them; without them nothing changes) and the PMP firmware running (nothing in the repository starts it);
  `perf_state` in the device's sysfs directory says what happened. See
  `receipts/2026-10-24-ane-level-driver/README.md`.

### M6 (T8152): module and device tree

- `ane/h18` gains `hs_props=1` (stage script argument 14, with chan 3 or 4). After CONFIG_GET it sends the five CH_PROPERTY_WRITE commands, in this order: property 0x10a4 = 1, then RESOURCE_INFO_GET, then 0x10a6 = 1, 0xa1 = 0, 0x10aa = 1 and 0x1209 = 1. The block is 0x14 bytes with the property id at +0xc and the value at +0x10. The last runs sent none of them and the first program stayed started and never finished. The values of 0x10a6 and 0x1209 depend on driver config bytes that are not known for the T8152, so both are sent as 1. A write the firmware refuses is logged and the sequence goes on. The default setting command (0x2e) is not sent, because its value comes from the same unknown config. The simulation checks the order, the ids and the values, a refused write and the refusal without `hs_start=1`.

- `ane/h18` logs the firmware console as text after a failed handshake command or a failed program. A record of the terminal ring holds the address of the text, its length and a timestamp (seen on a board in October 2026, not documented anywhere); the text lies in a buffer the host gave the firmware, so the module reads it and prints one line per record with the slot number and the timestamp. Records that point outside the module's buffers, or whose length is 0 or over 256 bytes, are counted and not printed. The first four printed records also appear as raw bytes (at most 32 each), to tell text from a format reference. Nothing is written. The simulation plants such records and checks the line. Not verified on a board yet: that every record on a real run has this layout.

- `ane/h18` device tree: the DART nodes of instances 1 to 3 now carry their ADT tunables. The kernel's `apple-dart` fails the probe of a Gen3 node without `apple,tunable` (`failed to parse DART tunables`, -ENOENT, seen on a board boot), so the default `t8152-j873-ane-dart.dts` of the previous change did not bring the ANE an iommu group. `t8152-j873-ane-dart-tunables.dtsi` is gone; the records are in `t8152-j873-ane-dart.dtsi`, and `t8152-j873-ane-dart-full.dts` only adds stream 11. `h18-dart-check.py` now fails a node without tunables. The earlier reasoning that the M2 reset values work without them did not apply: the driver requires the property. Not known: whether the 0x20c record on instances 1 to 3 changes translation on the Gen3 DART (instance 0 already carries it).

- `ane/h18` names the source of the PREMAP_BUFFER layout (macOS build 26A434). A malformed answer to PREMAP_BUFFER is a protocol failure now, and only a well formed refusal lets the program stage go on. The stage script asks for `dart=1` only when a DART instance mask is given.

- `ane/h18` gains `dart_inst` and `hs_premap` for the program stage. `dart_inst=<mask>` reads the ANE DART instances 0 to 3 read only before LOAD_PROGRAM and after the call. `hs_premap=1` sends PREMAP_BUFFER (0x20d, type 2) for one request pool before LOAD_PROGRAM, at init. The stage script takes both as arguments 12 and 13. The sim has five new cases and five mutants. Neither has run on the board yet.

- `ane/h18` device tree: `t8152-j873-ane-dart.dtsi` now has one DART node for each of the four ADT instances of `dart-ane` (instances 0 to 3) on the ADT interrupt 789, and the `ane0` node lists all four in `iommus` (stream 0). Before, only instance 0 was described, and the first M6 program ended with the engine not finishing and interrupt 789 stormed. `t8152-j873-ane-dart-sid11.dtsi` (stream 11 on every instance) is a separate file, joined in `t8152-j873-ane-dart-full.dts`, so a run changes one thing; the ADT records of instances 1 to 3 are in `t8152-j873-ane-dart.dtsi`. `ane/h18/h18-dart-check.py ADT.bin DTB` compares a built DTB with the ADT: the builds of `t8152-j873-ane-dart.dts` and `t8152-j873-ane-dart-full.dts` pass, and the previous overlay and a one-word tunable change fail. That the engine's DMA uses instances 1 to 3 is inferred from the M1 and M2 overlays and the M2 faults; nothing has run on the board.

- `ane/h18` with `hs_prog=1` and `read_dart=1` reads the exception state of the ANE DART before LOAD_PROGRAM and after the call. It only reads. It logs the error word and its address, the eight words of the bitmap of streams with a pending exception, and the record of the first eight streams with a bit set. The first program run on the Mac mini M6 needed this. The firmware answered LOAD_PROGRAM, CREATE_PROCESS and PROCEDURE_CALL for the H14 add program and posted one IO_T2H event with state 0. No finish event came within a second and the output buffer stayed zero. About 190 ms after the call the kernel logged an unhandled interrupt of the ANE DART. The kernel handler skips streams it does not own, so it printed nothing. Which stream faulted is not known yet.

- `ane/h18` gains `hs_prog=1` (needs `hs_start=1`): it loads a program file made by the new `tools/ane-prog-pack` from an ANEC, allocates a surface per section and buffer, sends LOAD_PROGRAM, CREATE_PROCESS and PROCEDURE_CALL on the IO ring in the T6021 wire format, reads the IO_T2H events, and unloads with TERMINATE_PROCESS and DESTROY_PROGRAM. The stage script takes `chan=4` and pins the program file. The sim has twelve new cases, eighteen parser cases and fourteen mutants. Whether the M6 firmware accepts the M2's program format has not been tried on the board.

### M3 (H15): module and overlay

- `ane_h15` stage 5 gains `run_scratch=1` (off by default): a read-only dump of the eight SCRATCH words before RUN and after the wait, one log line each. It writes nothing. Why: the first T6030 stage 5 runs ended with CPU_STATUS `0x300028` (STOPPED clear, IDLE set, the status a started M2 core reads), no mailbox message and `core-not-stopped`, and the firmware in memory is the 14.x generation, not the 27.0 one stages 6 and 7 were built from. On the M2 the start is followed by a wait for `0x08042006` in SCRATCH7; the dump shows whether this firmware writes it. The SCRATCH offsets are the T6030 ones and have not been read on an M3. `sim-test` checks that the dump reads each word twice, never writes, and that without the parameter the window is not touched.

- `ane/h15/t6030-ane-dart-experimental.dts` now enables the three ANE DART translation instances of the T6030 (0x309800000, 0x309810000, 0x309820000: DARTLLT, DARTBRD, DARTBWR in a J516S's same-boot ADT) and lists all three in the ANE node's `iommus` (stream 0); the aperture window at 0x309804000 stays disabled. Before, only instance 0 was bound, and the first T6030 stage 5 ended with the core leaving STOPPED, no mailbox message and `core-not-stopped`. Whether the other instances matter for that is not known: on the M6 the first message and the whole handshake worked with instance 0 alone, and the instances mattered when the engine ran a program. `tools/test_t6030_dart_overlay.py` (dt-overlays workflow) checks the overlay against the ADT facts and fails on four mutations. The README now says that clearing RUN does not stop a core whose firmware runs (M6 logs), so `core-not-stopped` alone does not say whether the core ran.

- `ane/h15/README-bringup.md` explains how to read the stage 4 DART output: `locked_windows` is bit 0 of PROTECT_LOCK (0x208), not of PROTECT (0x200); a T6030 tester's bank 0 PROTECT and PROTECT_LOCK `0x2` is bit 1, which on the M2 Max protects the aperture at bank base + 0x4000, the window the module skips as a different version. Carried over from the M2, not measured on an M3.

### Packages, docs and tools

- `docs/ane-constants-t6021-t8152.md` lists every numeric constant of `ane/t6021` with its T8152 (M6) value and, where known, its H15 value, each marked ADT, board, inferred or not known, and `tools/test_ane_constants_table.py` (run in the dt-overlays workflow) re-reads the T6021 values and the `ane/h18` and `ane/h15` references from the source. It records the places where the T6021 values cannot be reused on T8152: the wrapper, SCRATCH and doorbell offsets, the Postoffice mailbox, the firmware's channel table (TERMINAL has 0x200 slots, not 0x300) and the CPU_STATUS value after RUN.

- The S1 int8 package with GPTQ weights passes the quality gate on technical text and replaces plain rounding as the int8 package. On a 13-inch M1 the step p50 goes from 0.0863 s (fp16 package) to 0.0610 s, which is 16.4 tokens per second against 11.6, with KL 0.000397 and top-1 0.9958. Two decode streams give 18.05 scored steps per second against 12.12. On a 14-inch M2 Max the step goes from 0.1579 s to 0.1243 s with KL 0.000400 and top-1 0.9958. On the M2 Max GPTQ lowers the KL of plain int8 by 27 percent at the same step time. The package seals and the full table are in `docs/qwen-decode-packages.md`. The 16-inch M1 Max has not run this package.

- The int8 GPTQ package is an opt-in speed tier, not the default. On a 14-inch M2 Max it was also run on two prompt sets its calibration never saw, with the fp16 package in the same session. On 50 code prompts the KL is 1.57 times the fp16 KL (0.000454 against 0.000289) and top-1 is lower by 0.0013. On 50 long-position prompts of 170 positions the KL is 2.06 times (0.000604 against 0.000293) and top-1 is lower by 0.0028. The step is 21.7 and 21.0 percent lower. `docs/qwen-decode-packages.md` has the table. The default stays the fp16 package.

### Release and promotion tooling

- The release step cuts the release it should: one release per run covering
  every promotion merged since the last tag (verdict, chip, row shas,
  driver_source, merge sha), tagging the release commit so the tarball
  carries its own CHANGELOG section, with `## Unreleased` kept as the first
  section.

- The promotion flip is state-independent: the check's case lists and the
  tested-header list derive their order from rules instead of absolute seats
  into the v0.4.2 five-chip list, each overlay dts carries one
  tool-written `Overlay state:` line, the offline suites pick their chips
  from the overlays table, and `test_promote_chip` runs in the promotion
  gate. Any promotion state round-trips and passes.

- `propose` stages only the tracked files the flip wrote; `git add -A` had
  shipped a committed `__pycache__` blob in v0.4.3 (removed, and
  `__pycache__/` and `*.pyc` are ignored now).

- The T6000/T6020 prose matches 0.4.3: the README's default-on line and the
  evidence cells name the passing rows 6eb94f49985b and 3c9389040f51, the
  overlay dts headers and the libane receipt stop claiming untested/opt-in,
  and the libane entry and receipt carry the hardware numbers (#111) instead
  of "not merged before hardware" and lab aliases.

## 0.5.1 (2026-10-24)

This release turns the T6021 program unload on by default again. It adds START and RESOURCE_INFO_GET to the M6 module, answered on the board, and a shorter kernel log for its channel snapshots. It also adds the H15 channel stage and a C surface pack that `qwen_prog_run.py` uses by default. The lab decoders reach the pack only through a shim, so its measured gains are shim runs. The unload proof is one M2 and one workload. The H18 results are board measurements on one Mac mini M6, and the H15 stage has not run on an H15 part.

### T6021 (M2): program unload

- `ane_t6021` unloads a firmware program when its last user closes by default (`unload_on_close` is 1 again). On an M2, six decode sessions in a row passed: the logits were byte identical, the step time stayed within 0.1 percent, and `bo_total_bytes` after a pool drain stayed at about 54 MB. The table of firmware buffers stayed flat at 17 slots and 475,136 bytes. The two earlier runs that failed (the firmware's free request, the table of firmware buffers) are fixed in 0.5.0. The proof is one M2 and one workload with a pool drain between sessions. Killed sessions and other chips are untested, and `unload_on_close=0` restores the old behavior.

### M6 and H15 modules

- `ane/h18` gains `hs_start=1`, a fixed command order after the first two commands: PRINT_ENABLE, START, CONFIG_GET and RESOURCE_INFO_GET, with the 100 byte RESOURCE_INFO_GET reply logged. On the board all four were answered in one run with the command block inside the pool, and the core stopped cleanly afterwards. CONFIG_GET returned 0x16e3600, a 24 MHz tick rate. The board returned 0x80 at +0x18, 0x10 at +0x1c and 0x00200008 at +0x24. The names are assumed, not the board's: 0x80 procedures, 0x10 total cache request slots, and the two halves of +0x24 as 8 shared events (low) and 0x20 global wait events (high). The module does not use them yet. The stage script takes `chan=3` for it. The sim has two new cases and one mutant.

- `ane/h18` ring change snapshots are shorter. They print at most 4 nonzero lines per ring and leave out the terminal ring text. The explicit snapshots, after the channel step and after a failed command, still print everything. On the first board run with `hs_start=1` the six change snapshots overflowed the kernel log ring. The stage script then counted no new RESULT line and reported a failed step, although the module's own line said PASS. The log volume is lower now. Whether it is low enough on the board is unverified until the next run.

- `ane_h15` gains `stage=7` (`stage=chan`), behind `confirm_run=1`, `confirm_rtkit=1` and `confirm_chan=1` (and `map_fw=1`): stage 6 plus the SCRATCH handshake and channel step the M6 module needed to get past its stop wall (PRs 195, 196, 205). After AP power ON it reads SCRATCH0..7 (the T6030 offsets, never read on an H15 part), refuses unless SCRATCH7 reads READY, SCRATCH0 is below 0x21 and the firmware's IPC and HEAP requests are within limits, allocates POOL, IPC and HEAP surfaces in the ANE's default DMA domain (the firmware segments are mapped there, nothing is attached), fills the 0x174 byte init block, writes SCRATCH0, SCRATCH1 and the wake word, waits for the table address, checks all eight channel entries and that no ring overlaps the table or another ring before it writes anything into them, makes every host-to-firmware slot host owned, writes READY to SCRATCH3 and waits for the firmware to clear it. `chan_cmds=1` then sends PRINT_ENABLE and CONFIG_GET on the IO ring (the doorbell at engine + 0x1844000 is the only doorbell write). It writes SCRATCH0, 1, 7 and 3 only, never zeroes the SCRATCH words, frees its surfaces only after the core stopped, and the shutdown of stage 6 still follows. The sim has a firmware model (the SCRATCH words, a channel table, rings, the doorbell) and covers the exact SCRATCH write order, the block contents, ownership before READY, both command cases, and refusals: no confirm, no `map_fw`, SCRATCH7 not READY, SCRATCH0 too big, an oversized heap request, no table, a ring outside the surface, overlapping rings, SCRATCH3 never cleared, no or failing command answer, a failed allocation, no default domain, a stuck core; six mutants fail the harness. It has not run on hardware; run it only after stage 6 reached AP power ON on the same machine.

### Host tools

- `tools/surface_pack.c` and `tools/surface_pack.py` write a dense array straight into a prepared ANE input surface and read an output surface straight into a dense fp16 array. The numpy version zeroed a buffer, converted, assigned through a strided view, copied to bytes and copied again into the input buffer, and copied the output twice. The C version touches only the port's strided rows, because the padding of a prepared buffer is zero from creation. The bytes and arrays are identical to the numpy version, including fp32 overflow, subnormals, infinities and NaN, and `tests/test_surface_pack.py` checks that on seven shapes. On a 13-inch M1, in the S1 int8 decode (5 held-out prompts x 64 positions, each arm twice), the step p50 fell from 0.0650 to 0.0622 s (4.2 percent). The logits hashes of all five prompts were identical in every arm.

- `tools/qwen_prog_run.py` now packs and unpacks surfaces with `tools/surface_pack` by default. The bytes are identical to the numpy path, so the decode gives the same logits. It falls back to numpy when no C compiler is available, for a batch stride that is not the plane count times the plane stride, and when `SURFACE_PACK=0` is set. Bad strides still raise `Refuse`. The default reaches the code paths of this repository: `ane_call` and the resident session. The lab decoders (the 13-inch research decoder and the M2 int8 session) have their own numpy pack and use the C code only through a shim, so this default does not change them. With the shim on, the staged decode on a 13-inch M1 had a step p50 4.2 percent lower on the S1 int8 package and 2.5 percent lower on the shipped package, and on a 16-inch M1 Max 1.0 to 1.3 percent lower. Two tests of `tests/test_qwen_m2_decode.py` that failed on main because the stub decoder lacked `resident`, `session`, `resident_states` and `cpu_hist` now pass.

- The S1 int8 package with plain rounding stays an opt-in tier on both chips. On a 13-inch M1 it takes the step p50 from 0.0892 to 0.0649 s with KL 0.000540 and top-1 0.9944. On a 14-inch M2 Max it takes it from 0.1616 s (an older boot with the stock module) to 0.1233 s (a newer boot with the current module) with KL 0.000547 and top-1 0.9945. That is about 23 percent, but not a like-for-like pair: six sets of the shipped package on the newer boot read 0.1592 s without P-core pinning. The shipped package has KL 0.000267 to 0.000269 and top-1 0.9955. The quality gate is KL at most 0.000534 and top-1 at least 0.9945, so S1 misses it by 1.1 percent (13-inch) and 2.4 percent (M2 Max) in KL, and on the 13-inch also in top-1 (0.9944 against 0.9945). It is a speed tier for users who accept that cost.

## 0.5.0 (2026-10-24)

This release adds the bring-up of the M6 ANE (T8152) through the firmware handshake and the channel table, the T6021 program unload with the firmware's free request answered, the H15 stages up to the RTKit session, and an fp16 Q6_K head kernel. The unload and every H18 and H15 step are opt-in, and `unload_on_close` is off by default. One default changes in `ane_t6021`: EXEC and PROC_CREATE refuse a program id that the file did not load or look up. The H18 results are board measurements on one Mac mini M6. The H15 stages have not run on an H15 part.

### T6021 (M2): program unload

- `ane_t6021` has an opt-in `unload_on_close=1` that unloads a firmware program when the last file that loaded or looked it up closes. It terminates the program's process (command 0x203, programId at +8 and processId at +0xc) and destroys the program (0x201, programId at +8). It frees the section buffers only after both were acknowledged. The commands come from the selene 13.5 firmware. Command 0x201 halts the firmware if a process is still attached, so the process always goes first. Each file now holds a reference on the programs it loaded or looked up. `PROG_LOOKUP` takes that reference, and `EXEC` and `PROC_CREATE` refuse a program the file holds no reference on (`ENOENT`). The option is off by default and keeps the old behaviour: the first run with it on, on an M2, failed at the first terminate-process command and left the device quarantined until reboot. A failed unload leaves the program held for good. The program table and the unload rule live in `ane_t6021_progtab.h`, with a host unit test, `tools/test_t6021_progtab`. Nothing has run on a device yet.
- `ane_t6021` answers the firmware's request to free a shared-memory buffer. The first run of the program unload on an M2 failed because the driver took that request, which the firmware posts on its allocation ring while it terminates a process, for a malformed allocation. The firmware waits for the answer with no timeout, so the driver now answers it (body zero, header 1, doorbell bit 5) wherever it serves the ring: before each command, while it waits, and for 10 ms after a terminate or destroy command. A host test classifies the ring entries and five mutants fail it. Not run on a device. `unload_on_close` stays off until an M2 proof passes twice.
- `ane_t6021` gives back a buffer the firmware returns. A second M2 run of the program unload passed three sessions with the free request answered, and then the fourth and fifth sessions failed in the middle of a LOAD. The driver kept every buffer the firmware had asked for in a table with a budget of 512 MiB and answered a FREE without releasing the entry, so each block the firmware freed and asked for again used a new entry. A FREE now matches the returned IOVA to its table entry (never the command buffer), frees the buffer, credits its bytes and leaves the slot for the next request, and every FREE log line prints the table's high-water mark and byte count. The slot logic is a pure header with a host test and four mutants. The cause is not yet confirmed by those numbers on a board, and `unload_on_close` stays off until a run of six sessions passes.

### Host tools

- `tools/q6k_head.c` gains `q6k_matvec_f32_fp16x`. It is a Q6_K head kernel that multiplies in fp16 and accumulates in fp32 with FMLAL. `tools/q6k_head.py` uses it when the CPU has FP16FML and the activation vector is already an fp16 value. The head input of the staged decodes is the fp16 output of the engine, so the rounding changes nothing. The lanes are summed in the same order, so the result is bit-identical to `q6k_matvec_f32`. Any other input takes the old kernel. On a 13-inch M1 the kernel alone takes 11.8 ms instead of 15.9 ms for the full 248320-row head with 4 threads. In the decode (one stream, 5 held-out prompts x 64 positions) the logits hashes were identical with and without it. The rate went from 12.19 to 13.09 steps/s with the S1 int8 package (+7.4 percent) and from 8.96 to 9.44 steps/s with the shipped package (+5.4 percent). `Q6K_FP16X=0` turns it off. The tests run the new kernel on aarch64 hosts with FP16FML and skip elsewhere.
- The strict port-table reader of `ane-run` and `ane-session` accepts a trailing unknown member. Before, a string or a number placed last in the top-level object made the whole table malformed. The skip of an unknown value never stopped at the closing brace. An M2 session refused a LOAD with `bad-ports` for a package whose port tables carried one such key. `tools/test_port_reader.sh` extracts the real reader and checks nine tables. It fails on the old code for the three trailing-scalar cases.

### H15 bring-up (T6030, never run on an H15 part)

- `ane_h15` gains `stage=4` (`stage=dart`, behind `confirm_read=1`, read only) and `stage=5` (`stage=run`, behind `confirm_run=1`). Stage 4 answers the M6 lesson first: on the M6 (T8152) a RUN write started the core and it posted nothing, and nobody had read the ANE DART, so whether the boot loader leaves a stream enabled is unmeasured. A core with no enabled stream could not fetch its firmware. Stage 4 reads the ADT's four ANE DART windows (the T6030 DART is `dart,t8110`, the register file `apple-dart` drives): version, lock state, ENABLE_STREAMS, TCR and TTBR of streams 0 to 63, and reports `live_streams`. It shows whether the boot loader left a stream enabled and translating. It does not show that the mapping covers the firmware or that the core would run. Stage 5 is the first stage that writes. A 16-inch M3 Pro (T6030) owner ran stages 0, 1 and 2 (PR 175): all five power domains reached ACTUAL=0xf, CPU_STATUS read 0x0030002a, RVBAR read 0x0000010000000001 and both mailbox CTRL words read 0x20001. Stage 5 uses those numbers and refuses on any SoC row without a measured RVBAR (`rvbar_expect`, only `t6030` today). It also refuses unless the stage 4 scan finds a live DART stream (`run_unmapped=1` overrides). It refuses unless the core is stopped, CPU_CONTROL RUN is clear, RVBAR is exactly the measured value and both mailbox words read EMPTY. It then sets CPU_CONTROL bit 4, watches CPU_STATUS and the I2A mailbox for `run_wait_ms` (default 300), logs at most four raw I2A messages without replying (one 64-bit read per receive register: they pop on read, and a 32-bit read would take the message apart), and clears bit 4 then bit 5. A core that does not report STOPPED afterwards keeps its power claim, `remove()` refuses, and the module takes a reference it never drops (`__module_get`), because `remove()` is void and cannot stop an `rmmod`. Stage 3 is unchanged. Stage 5 writes only CPU_CONTROL bits 4 and 5, never RVBAR, the mailbox or the DART (the ANE DART nodes stay disabled; the DART mapping is a later stage). Stage 5 also takes `map_fw=1` with `fw_seg=TEXTPA:SIZE,DATAPA:SIZE`: it attaches the ANE to its DART through the iommu API and maps the two preloaded firmware segments (TEXT read only at the RVBAR entry, DATA read+write behind it) before RUN, the pattern of the M6 module (PR 185), only on a core whose CPU_STATUS reads stopped, refusing segments that are unaligned, over 64 MiB, overlapping or inside system RAM, and undoing the mapping after a failed step or a clean stop. `ane/h15/t6030-ane-dart-experimental.dts` enables DART instance 0 and gives the ane node `iommus`; `ane/h15/h15-segments.py` turns a live ADT dump into the `fw_seg` value (checked against the saved M6 ADT). On the M6 the board showed every ENABLE_STREAMS word 0 before the map (PR 185). The userspace sim covers the stages with a device model (STOPPED, the mailbox, the DART registers, the iommu calls). Sixteen mutants are killed: confirm ignored (run and dart), RVBAR check skipped, mailbox check skipped, RUN never cleared, a stuck core accepted, a 32-bit message pop, the DART gate skipped, a stream counted live without a valid TTBR, the module left unpinned after a stuck core, the map never undone, the stopped check skipped, the RAM check skipped, TEXT mapped writable, overlapping segments accepted, `iommus` not required. The module builds with `W=1` against the omarchy-linux tree and warns about nothing. It has not run on hardware, and the DART overlay has not been booted. Sources and what is inference: `ane/h15/run-stage-sources.md`.
- `ane_h15` gains `stage=6` (`stage=rtkit`), behind `confirm_run=1` and `confirm_rtkit=1`: stage 5 plus the RTKit management session the M6 module runs, over the ASC mailbox of the H15 (send registers +0x800/+0x808, receive +0x830/+0x838, the apple-mailbox v4 layout, inferred for the ANE and never written on an H15 part). After RUN it answers HELLO (version 12) and the endpoint map, starts the announced system endpoints, answers the IOP power ON acknowledgement with AP power ON, starts the application endpoints, and shuts the session down (`rtkit_stop=1`: AP QUIESCED then IOP SLEEP, restartable; `2`: OFF then OFF) before RUN is cleared. It writes only the two send registers (16 messages at most, never while A2I reads FULL, management endpoint only), answers no buffer request and no message of another endpoint, and the verdict says whether the firmware said nothing, the session or the shutdown was incomplete, or the core did not stop (module pinned, reboot). The sim has a firmware model (HELLO, endpoint map, crash log request, power acknowledgements) and covers the exact message sequence for both stop modes, with `map_fw`, and the refusals, a silent firmware, an incompatible version, a full mailbox, missing acknowledgements, a map flood and a stuck core; six mutants fail the harness. It has not run on hardware; run it only after a stage 5 result with a mailbox message in it.

### H18 bring-up (T8152, Mac mini M6)

- `ane/h18` is an opt-in bring-up module for the ANE0 of the T8152 (Mac mini M6). It has three stages and no firmware path. `stage=dt` checks the device tree windows against the SoC row and touches no register. `stage=psd` reads the two PSD slot windows (slots 30 and 31, the ADT power gates of the ANE) and writes nothing. `stage=power` needs `confirm_power=1`. It requests a PSD state on slot 30 (and 31 with `psd_slots=2`), waits for each acknowledge the way the kernel PSD driver does, and only then reads CPU_STATUS, ASC_STATUS and IORVBAR. It reads CPU_CONTROL only with `read_ctrl=1`, never writes it, and writes nothing in the ASC windows. Every address is logged before its access. The live M6 ADT shows that the boot loader preloads the firmware (`segment-ranges` of `/arm-io/ane`), so a later change maps it and releases the core. On a Mac mini M6 request 1 on slot 30 was acknowledged, the ASC wrapper then answered with CPU_STATUS 0xa and IORVBAR 0x10000000001, and the slot returned to state 6. Whether the core runs is not known. The mailbox channel is not verified either, so `read_mbox=1` reads three candidates. `stage=boot` (not run on any machine) starts the core with the RUN bit only when the core is stopped, RUN is clear and IORVBAR is the value the boot loader left, watches it for a short time, reads at most 8 mailbox messages without sending any, and clears RUN again. The module builds W=1 clean against the M6 kernel tree. The userspace sim has positive and negative controls and five mutants that must fail.
- `ane/h18` stage boot gains three opt-in parameters. `alias_mbox=1` watches the alias-bank Postoffice channel of the ANE. That channel is at window + 0x10000, channel 2 * mailbox number. The module reads its status word once before RUN. `read_dart=1` reads twelve words of the ANE DART before RUN with both PSD slots on. It writes nothing. `rtkit_hello=1` answers the firmware's RTKit HELLO with version 12, and answers its endpoint map. A reply needs an inbox credit, a message for endpoint 0, and a firmware version range that includes 12. The stage script takes `dart` and `hello` as arguments 7 and 8 of step 4. The sim covers both halves as outbox, the credit, version and endpoint refusals, and five new mutants.
- `ane/h18` stage boot gains `map_fw=1`. It attaches the ANE to its DART through the iommu API and maps the two firmware segments the boot loader left in DRAM at their IOVAs before RUN. The board showed that the boot loader leaves no DART stream enabled, so the core cannot fetch its firmware. A new overlay, `t8152-j873-ane-dart.dtsi`, adds the ANE DART, a PSD group for slots 30 and 31 and the links in the `ane0` node. With power-domains in the node the module requests no PSD state and holds a runtime PM reference instead. The stage script gets step 5, which is step 4 with `map_fw=1`. The sim covers the attach, the maps, the undo on each failure and four new mutants.
- `ane/h18` stage boot gains `rtkit_start=1` and `rtkit_stop=1`. The board showed the ANE firmware sending the RTKit HELLO and two endpoint map messages once its firmware was mapped through the DART. `rtkit_start=1` starts the system endpoints after the endpoint map. It answers the firmware's IOP power state acknowledgement with AP power state ON, and starts the application endpoints after the AP acknowledgement. `rtkit_stop=1` runs the restartable RTKit shutdown (AP power state QUIESCED, then IOP power state SLEEP, as apple_rtkit_shutdown does) before RUN is cleared, and `rtkit_stop=2` runs the OFF shutdown, the pair that stopped the core on the board. The stage script takes `hello` values 2, 3 and 4 for them. The sim has a firmware model that reacts to the messages the host writes, and two new mutants.
- `ane/h18` stage boot gains `read_scratch=1`. It reads the eight SCRATCH words of the engine block, the registers of the macOS boot-args handshake, three times: before RUN, when AP power ON is acknowledged and before the shutdown. It writes nothing. The stage script takes `scratch` as argument 9 of steps 4 and 5.
- `ane/h18` queues an RTKit management message that finds no free TX slot instead of refusing it. Before, a reply to an endpoint map or an endpoint start could be lost when the firmware had not yet freed a slot. The queue holds eight messages and is stored in order at every poll, and the shutdown waits for it to drain. The userspace sim has a mode where the only slot returns one poll after each message, and a mutant that drops the queued message fails it. The board has not needed it: the firmware freed the slots in time on every run.
- `ane/h18` stage boot gains `handshake=1`, the first option that writes the SCRATCH words. After AP power ON is acknowledged it allocates the IPC, HEAP and init block surfaces, publishes the block address in SCRATCH0 and SCRATCH1, writes the wake word to SCRATCH7 and waits for the firmware's table address. The layout is the T6021 init block, which the M6 driver shares. It needs `map_fw=1`, `read_scratch=1`, `rtkit_start=1` and a shutdown, and it maps the firmware into the default DMA domain. The sim has eight cases and eight mutants. On the board the firmware answered in 2 ms with its channel table of eight entries, the T6021 names and bits. The pool surface is 0x40000 bytes as on T6021, the handshake refuses unless SCRATCH7 already shows READY, and the template word is a zero guess named in the README.
- `ane/h18` stage boot gains `hs_chan=1` and `hs_cmds=1`, the steps after the handshake. `hs_chan` checks the firmware's channel table, hands the host-to-firmware rings to the host and writes READY to SCRATCH3 after it created its channels. `hs_cmds` sends PRINT_ENABLE and CONFIG_GET on the IO ring and rings the doorbell at engine + 0x18a4000. On the board the firmware cleared SCRATCH3 within 1 ms, and the OFF/OFF stop then completed, the first stop that did after a handshake. The first command, PRINT_ENABLE, got no answer, and the doorbell address is still unconfirmed. The sim has eight cases and six mutants.
- `ane/h18` `hs_chan` serves the firmware's SHAREDMALLOC ring and logs what the firmware posts. The first board run of `hs_chan` and `hs_cmds` cleared SCRATCH3 in 1 ms and let the OFF/OFF stop complete, but the first command got no answer. The module now answers a MALLOC with a DMA buffer and a FREE with the answer the firmware waits for, acknowledges a pending IPI at engine + 0x18b0000, and logs the nonzero lines of every firmware-to-host ring after SCRATCH3, after a failed command and whenever a ring or the pending value changes. The command block now lives inside the pool surface at +0x400, and the terminal ring is logged as text. The sim has four new cases and eight new mutants. This step has not run on the board yet.

## 0.4.10 (2026-10-23)

- `ane_h15` attaches each of the ANE's five power domains by id and links them with `RPM_ACTIVE` device links, the pattern `ane_t6021` uses (#173). Before, only the driver core's automatic attach ran, which handles one domain, so stage 1 stopped at `pm_runtime_get_sync` with `-EACCES`. The contributor reports stage 0 and stage 1 passing on a 16-inch M3 Pro (T6030). We did not run it on hardware.
- `tools/test_ane_smoke.py` covers die reporting for the H13 and H14 Ultra module families (#178). It is a test-only change.
- The `dt-overlays` host checks now trigger on every tool input, and a failing regression binary can no longer hide behind `| tail` (#179).
- Aurora CI builds the resolved revision of `aurora-wip` instead of the moving branch, and `asahi-dtbs` checks out the requested ref on a restored cache (#180).
- `packaging/omarchy-ane-smoke` no longer raises `AttributeError: 'str' object has no attribute 'is_file'` when `omarchy-ane-run` is found only on PATH. `which()` returns a string, and `tool()` called `.is_file()` on it. The packaged layout hid the bug, because the runner sits beside the script there. A user who runs the smoke from a checkout with the runner on PATH got `smoke: attempted, available false` in the collector row instead of a result. `tools/test_ane_smoke.py` has a case for a runner found only on PATH.
- The three H15 overlays in `ane/h15` (`t8122`, `t6030`, `t6031`) now ship their four ANE DART nodes as `status = "disabled"`. A T6030 owner found that equivalent enabled nodes make the DART driver probe unpowered ANE hardware at boot and hang the machine before display and network. That was not measured with these exact overlays. The `ane` node loses its `iommus` property in the same change, because `omarchy-ane-dt apply` refuses a node that names a disabled provider. Stage 0 and stage 1 of `ane_h15` need no DART. The change is untested on a T8122 and a T6031. The overlays compile with dtc 1.7.2.
- `ane/h15` stage 2 (`stage=wrapper`) now has a read-only mode, `confirm_read=1`. After the same ACTUAL=0xf gate as stage 1 it reads CPU_STATUS, RVBAR and the two mailbox CTRL words of the engine window. Each address is logged before its read. Before any bus access it compares the live reg[0] and reg[1] resources with the SoC row and refuses on a mismatch. It only reads inside the two windows the ADT publishes for the ANE node, and it refuses CPU_CONTROL, the CoreSight line and the T6021 fatal range whatever the SoC row says. Without `confirm_read=1` the stage refuses as before. It writes nothing and releases no CPU. The result line compares CPU_STATUS with the value a stopped T6021 core reads. The meaning of those bits on H15 is not measured. Nothing in this change ran on an M3 chip. The userspace sim covers all four SoC rows and has three new negative controls and four new mutation controls.
- `ane/h15/h15-capture.sh` records the device tree facts that the boot stage needs, without touching a register. It prints the firmware version string, the ANE, DART and mailbox nodes with their interrupt lines, the reserved firmware regions with their iommu addresses, and the interrupt and genpd lines. It does not fill the unmeasured fact H3, because the ADT segment-ranges and the DART geometry are not in the Linux device tree. It loads no module and writes no file.
- `tools/qwen_fold_layout.py` builds the manifest of a folded staged Qwen layout from the control manifest and the compile record of that layout. A fold joins the DeltaNet state block of a layer to the program after it. The tool checks on the host that every unfolded program has the control program's MIL, that every folded program is the state block followed by the next control program, and that its output lanes and states match. Every folded program must also be the chain of those two control programs op for op, checked with `tools/mil_graph_equiv.py` with the lanes labelled, so a stale or miscompiled fold with the right ports is refused. With `--self-check` it rebuilds all 38 control entries from the control's own record and compares them with the control manifest. All 38 are equal. Its 37 program layout equals a manifest built by hand for the same fold.
- `tools/qwen_fold_set_compare.py CONTROL_SUMMARY LAYOUT_SUMMARY LAYOUT_MANIFEST` reads the two boots of a one-arm run. It reports the exec time saved per fold, the sum of the replaced control programs' exec medians minus the folded program's, with the range over each boot's arms as noise. It also reports the drift of the programs both layouts share between the boots, and the step ratio against both spreads. The step ratio is a screen, because drift between boots is in neither spread. It exits 2 when a summary is not a frozen run or lists failures, and when the layout run recorded a different control summary hash than the one given.
- `tools/qwen_fold_set_ab.py` runs the control and any number of folded layouts in one decoder process, one session and one boot. A custom `--order` must hold complete rounds, one control followed by every layout. It checks that every arm decodes the same token ids on ten prompts, reports the fp32 logits difference, and times the arms in rounds. The summary gives each layout's step time against the control arm of the same round, the control's own spread, and the exec time saved per fold. A difference counts only when it is larger than that spread. The ANE buffer window cannot hold the control and a 20 program layout together, because each takes about 3.1 GiB of it in power-of-two size classes and sections are held until reboot. `--only ARM` runs one arm per boot, a layout against the control ids and logits hashes of the other boot. The refusal counts the size classes against 3.5 GiB, so the control with a 20 program layout is refused before any load. A manifest built without the chain check is refused. The harness loads the large sections first and pins itself to the performance cores. There is no speed gate. Tests in `tools/test_qwen_fold_set.py` use synthetic records and cover the builder checks, the spread rule and the integrity gates.

## 0.4.9 (2026-10-16)

This release holds host tools and tests for the staged Qwen decode on the Apple M2 Max (T6021), one longer driver message, a NixOS lookup in the packaging scripts, and receipts for the Parakeet encoder on the M1 family. The driver calls and the libane interface are unchanged from 0.4.8. The decode launcher now pins itself to the performance cores. The Q6_K head is off by default. No decode speed gain from the Q6_K head is measured yet, because the decode run with the flag has not happened. The GitHub release notes list the measurements and their limits.

- `tools/anec_compare.py A.anec B.anec` compares two H14 ANEC packages section by section on the host: the header counts, the task stream, the constant section and the tile table. It also says whether A's constants occur as one run inside B's, which a fold that keeps its weights and adds a few bytes passes. This is the check `mil_graph_equiv.py` leaves out, because that graph check does not compare constant bytes. On the staged Qwen fold pair it shows what the fold kept and changed. The fused program has the same constant section as the readout, MLP and input-block program it absorbed, byte for byte (117,980,352 bytes). Its task stream differs, with 61 tasks against 52. The tool exits 0 when the packages are equal, 1 when they differ, and 2 for a file that is not a readable package. `--constants-equal` requires only the constants to match, and `--json` prints one object. `tests/test_anec_compare.py` uses the state block fixture and packages built from its bytes. It covers each section, a fold that adds bytes, damaged files and the exit codes. Host only, nothing runs on a device.
- `tools/qwen_m2_decode.py --q6k-head` (default off) reads the tied head through `Q6KHead` instead of the dequantized fp32 head: `embed[token]` is the Q6_K row dequantized as ggml does and rounded to fp16 (bit-equal to the fp16 table built from the fp32 head), `head @ hidden` is `Q6KHead.logits` with an fp32 activation, one thread per performance core. The weight values are the same; only the fp32 summation order of the logits differs, so logits are not bit-identical to the BLAS head. A GGUF whose `token_embd.weight` is not Q6_K is refused. The start record in `results.jsonl` says `head: q6k` or `head: fp32`. Offline test: `tools/test_qwen_q6k_hook.py` (a tiny GGUF made from the 64-row real fixture; embedding rows bit-equal for 64 tokens, logits within 1.7e-6 of the fp32 head, a non-Q6_K head refused). `tools/qwen_q6k_gate.py` is the host-only gate on real data: the hidden vectors of a sealed decode (recovered from its fp32 logits) must give the same token ids, and a logit difference below a tenth of the smallest sealed margin; it also times the two heads in the decode's own pattern (146 ms idle between steps), A B A B. The default stays the fp32 head until a decode run with the flag has passed the identity gate.
- `tools/q6k_head.c` and `tools/q6k_head.py` read the tied output head of the staged Qwen decode as Q6_K and multiply it by an fp32 vector.
- Today the host reads that head as fp32, 2.03 GB every generated step. On the M2 that is the memory ceiling, 95.6 GB/s measured. The GGUF stores the tensor as Q6_K, 417.2 MB. The kernel reads a fifth of the bytes and dequantizes in NEON registers.
- `q6k_dequant_row` is bit-identical to the fp32 head in use. Its sha256 on 64 real rows equals that of gguf-py's dequantization.
- `q6k_matvec_f32` keeps the activations in fp32 and factors the group scale out of the sum. Logits differ from a BLAS sgemv at fp32 rounding and are not bit-identical. On 2,001 real rows against a float64 dot, its largest error is 1.4e-6 for logits up to 5.8. Numpy's own fp32 matvec gives 1.8e-6.
- `tests/test_q6k_head.py` has 13 tests. The scalar path runs natively, and the NEON path is cross-compiled for aarch64 and run under qemu-user. Five mutants of the lane map and the bit decode are killed.
- No speed is claimed, because the NEON path was not timed on a chip. `tools/q6k_head_bench.py` times it on the real tensor, on CPU and memory only. It checks the first 256 logits against a float64 dot for every thread count and exits 3 when an error is not finite or is above `--tolerance` (default 3e-6). The decode launcher does not use it yet.
- The T8112 refusal line now carries the PWGATE word and the `ane_cpu` state: `PWGATE closed (word 0x…, ane_cpu 0x…); refusing before any engine read`. Before, the word was only in a `dev_dbg` line, so a first run on a T8112 needed `dyndbg` to show it (issue 160). The driver still only reads the gate; nothing writes it, and the refusal itself is unchanged. A window that cannot be mapped reads as `0xffffffff` and the line says so. The line was compiled for arm64 against the 7.1.12 headers (no load, no run on a T8112), and it is a message change only.
- `tools/hwx_ports.py --program NAME --hwx FILE --anec FILE --mil FILE [--out FILE]` derives the port table of one program. The mode that rewrites the 38 staged Qwen tables is unchanged. For the staged DeltaNet state block it equals the table in use on the M2. That covers ports, DMA coverage, ambiguity groups and exceptions.
- `tests/test_hwx_fold_tools.py` and `tests/fixtures/hwx-fold/` add a real Apple H14 program to the converter tests: the DeltaNet state block of the staged Qwen decode. The converter emits the committed ANEC for it byte for byte. A recompile of the staged programs 001 and 002 on a Mac, converted here, matches their staged ANECs byte for byte. The test runs the same check on the staged HWX when it is present. The HWX held a build path in its compiler command line. The fixture replaces it with a same-length placeholder. The converter does not read it, and the ANEC is unchanged.
- `tools/mil_graph_equiv.py` checks that one fused MIL program is the chain of two programs. It compares the op graphs with the function arguments labelled by lane, so a fused program that swaps q and k is not equal to the chain. The regression test runs it on the staged state block, the readout and MLP program that follows it, and the single program that replaces the pair: 136 ops on each side and all nine output lanes equal. It also fails on a swapped lane and on a changed op. It does not compare constant bytes, and it says nothing about what a chip does with an intermediate that no longer crosses a program boundary.
- `tools/pcore_mask.py` keeps an ANE host process on the performance cores. On the M2 a decode thread and its ane-session child that sleep most of each step were placed on efficiency cores. Every call then cost 0.72 ms more with the exec time unchanged. All 11 steps whose calls all ran there were slower than every step on the performance cores, by 27.0 ms in the ANE phase on average. They are the first step and ten more. No call of a pinned arm ran there. The helper reads the performance cores from `cpu_capacity` (the CPUs at the maximum), so no chip list is hard-coded. A chip without capacity files, or with one capacity, is left alone. `pin_self()` runs at the top of a launcher before any thread exists, so threads and the session child inherit the mask. `pin_tasks(PID)` re-applies it to a running process, and `OMARCHY_ANE_NO_PIN=1` turns it off. The command line prints the list in `taskset -c` form. A pin changes no arithmetic. Tests: `tests/test_pcore_mask.py`, host only.
- `tools/qwen_m2_decode.py`, run as a script, now pins itself and its `ane-session` child to the performance cores through `pcore_mask.py`, and sets `OPENBLAS_NUM_THREADS=4` when it pins and the variable is unset (`OMARCHY_ANE_NO_PIN=1` turns off both). Every run records the placement: a `placement` record at the start and at the end of `results.jsonl` (the end record is also written when a failed call or a kernel STOP line ends the run) (the mask of every task of the decode process and the child, the calls by CPU, the calls off the performance cores), a `calls_cpu` field on each step record, a sixth `timings.tsv` column with the CPU of the decode thread after each call, and a stderr warning when a call ran on an efficiency core. A harness that imports the module is not pinned. The outputs do not depend on the mask. Offline test: `tools/test_qwen_placement.py`. The M2 evidence is `receipts/2026-10-15-m2-qwen-pin-pcores/` (one boot, one session; the registered speed gate failed on its A/A clause, and every one of the 10 generated steps with all calls on efficiency cores was slow; so was the prefill step 0 of that arm, which also ran all 38 calls there).
- Receipt `receipts/2026-10-11-ane-gpu-cross-effect/`: on the 13-inch M1, a Parakeet encoder loop on the ANE and a compute-bound fp16 matmul loop on the GPU running together keep 95.1 percent of the GPU's solo rate (0.5509 against 0.5791 TFLOP/s) and run the ANE loop 2.9 percent slower (141.47 ms against 137.51 ms), in a quiet window under the exclusive-window protocol with alternating phases; each of the four ANE processes exited 0 with its final output matching the gold. One run, one workload, one boot; a memory-bound GPU job was not measured.
- `ane-run --tile-shift N` (1 to 20, index mode only, M1-family path only) sets the tile-count unit of the program header as a power of two. The whole-program containers written by the hwxv2 converter count tiles in 512-byte units (`--tile-shift 9`). The default is unchanged (0x4000-byte units), so existing callers behave as before. Without the option the Parakeet whole encoder (458,018,816 bytes) is sized 32 times too large, about 14.7 GB, and on the 13-inch M1 both runs failed with `DRM_IOCTL_ANE_BO_INIT failed`. With `--tile-shift 9` six runs on the same machine were bit-exact against the reference output, with medians of 20 calls from 137.4 to 158.2 ms (receipt `receipts/2026-10-11-m1-parakeet-tile-shift/`). Offline test: `tools/test_ane_run_tile_shift.sh` (part of `make check`). It guards option parsing, not the delivery of the shift.
- Receipt `receipts/2026-10-11-m1-parakeet-operating-point/`: the Parakeet whole encoder on the 13-inch M1 takes 137.914 ms (median of five 20-call medians, best 137.789 ms, spread 0.18 ms, bit-exact against the macOS gold) under the exclusive-window protocol. The paired macOS median on the same chip is 111.56 ms, so the cell is 0.809x (macOS arm at load average 2.76). With 8 CPU spin loops beside it the same program takes 146.05 ms (two runs). With another lane's GPU job running it took 161.3, 158.3 and 140.9 ms (three runs, not a controlled load) and 165.0 ms with a spin load added. The M1 Max runs the same program at 431 to 439 ms under every condition tried, including every CPU cluster capped at 600 MHz. Scoreboard cells should be taken in an exclusive window. Includes the 10 Hz state sampler and the exclusive-window loop.
- `ane-run --check matvec` and `--check rms` passed any output for a lane whose exact sum of absolute terms is 0, such as an all-zero input row. The error in condition units was set to 0 for that lane, so an all-ones or NaN output counted as in band. Such a lane now passes only on an exact 0 and reports an infinite error otherwise. For `--check rms` the reference of an all-zero input row was 0/0 = NaN, so even an exact all-zero output failed; the reference for such a row is now 0. The rules are in `tools/ane_check_band.h`. `tools/test_check_band.c` runs on the host, is part of `make check`, and failed on the old rules for the zero-sum and all-zero-row cases. It tests the lane rule and the rms reference; `rms_check` itself needs a device program and was not run.
- `ane-run --time` now also prints the mean of the per-call exec times, and `--time-raw FILE` writes every call's exec time in nanoseconds, one integer per line, in call order (it implies `--time`). The exec time is the wall time of one `ane_exec` ioctl: the hardware-polled completion timestamp as the host sees it, which includes the driver's detection lag, not the engine's compute time. When calls land on steps of about 51 us, a quantile can only sit on a step, so a change smaller than a step shows as a one-step jump or not at all; the mean and the raw values do not have that limit. The helpers are in `tools/ane_timing.h`; `tools/test_ane_timing.c` runs on the host (no device) as part of `make check` and covers the mean, the write, the call order and an unwritable path. The summary line only gains a trailing `mean` field.

## 0.4.8 (2026-10-10)

This release is for the Apple M2 Max (T6021). It also holds the v0.4.7 changes below, which were tagged without a changelog heading. The GitHub release notes list the hardware evidence and its limits.

- `ane_t6021`, `libane`: new `DRM_IOCTL_ANE_PROG_LOOKUP` (0x7, appended to the
  uAPI). A process asks the driver for a program by the SHA-256 digest of its
  sections. If the driver already holds that program, the process skips the
  section allocation. A kernel module that lacks the call, or a miss, falls
  back to the old load path. Measured on one M2 Max, module build `bd34a12`
  (#141, `receipts/2026-10-10-m2-max-prog-lookup`): three passes over the
  38 Qwen3.8-2B programs, one process each, loaded 38 of 38 every time. The
  allocator total grew by 2,416,623,616 B, 196,608 B and 0 B. On `ca09ce8` the
  second pass failed 5 times with `BO_INIT failed`. One run, zero-valued
  inputs.
- `tools/qwen_m2_decode.py --resident`, `ResidentSession` and the proxy A/B
  harness, with host tests (#144). A resident decode on `bd34a12` gave logits
  that equal the per-call baseline (`receipts/2026-10-10-m2-max-resident-qwen-decode`).
- Receipts for the Parakeet whole encoder on the M2 Max (#138, #142).

- `tools/qwen_m2_decode.py` warns at the start of a run when numpy uses the reference BLAS, and
  `docs/qwen-m2-decoder-requirements.md` says to install `blas-openblas`. Measured on the M2 Max
  (2026-10-10, module `bd34a12`): the output projection took 604 ms per generated token on the reference
  BLAS (any thread count) and 20 ms on OpenBLAS; the resident Qwen3.8-2B decode went from a generation
  step p50 of 0.826 s (1.21 tokens per second) to 0.211 s and 0.226 s in two runs (4.64 and 4.48), with
  the same decoder files and the same 16 generated tokens. The float32 logits differ at the rounding
  level (largest difference 4.0e-5, so the old logits hash no longer matches); the new hash is the same in
  both runs. Receipt: `receipts/2026-10-10-m2-max-resident-decode-openblas/`. Verified the same package
  swap on an M1 Max and an M1 (matvec 21 ms and 47.5 ms).

- `docs/ultra-test-runbook.md`, `docs/h15-volunteer.md`: audit against the
  code they describe. Opt-in path is `/etc/omarchy-mac-boot/dtb-overlays.opt-in`
  (`omarchy-ane-dt` has read that since 2026-10-04; the runbook still named the
  `omarchy-platform` one). Stage 2 names the die-1 smoke honestly: the shipped
  smoke measures the runner's default device and its `die` field is computed
  from the first bound node, so the die-1 run is `ANE_DEVICE=1` and the report
  must say which run each JSON came from; the stop rule adds the die-1 mailbox
  (0x2285408000), which `promotion_check.py` counts as an ANE line; the
  `ultra-die1.md §9.2` anchors point at §9 fact 2, the only numbering that
  section has. `h15-volunteer.md`: the kernel fork is
  `joshuaswarren/aurora-linux-tbnet` (branch `ane-driver-aurora` at `efe6e359`
  re-verified via the old name's redirect), and the collector-not-wired check
  is re-dated 2026-10-10.

- `ane_t6021`: the parked-BO pool never parked anything: the pool entry's
  size was never recorded, so every admission check saw 0 bytes and every
  freed duplicate section was really freed (measured on the M2,
  2026-10-09, boot 6fcfac08: `bo_pool_bytes` stayed 0 through a whole
  A/B). `ane_t6021_pool_park()` and `ane_t6021_pool_park_uncharged()` now
  take the size and record it on the entry themselves, so the caller
  cannot forget it.
- `tools/ane-session`: a LOAD whose (anec bytes, ports bytes) pair is
  already loaded rebinds the name with no device work; FREE drops the
  name binding only and the cached program dies at QUIT. A configure
  pass that FREEs and re-LOADs the same 38-program table now costs zero
  `BO_INIT`s (measured 2026-10-09: every pass's duplicate sections had
  asked for a fresh 223 MiB dma32 hole the window no longer had).
- `tools/ane-bo-probe`: each step now prints the power-of-two IOVA size
  class the request lands in (the dma32 IOVA allocator rounds and
  naturally aligns every coherent allocation to that class, so a
  223 MiB BO needs a free 256 MiB window).
- `ane_t6021`: a freed duplicate section BO of at least `bo_pool_min_kb`
  (default 2048 KiB) now parks in the BO pool under a runtime budget
  `bo_pool_max_mb` (default 512 MiB, 0644), evicting the oldest park by
  real free. `BO_INIT` of the same page-aligned size gets the same dma
  range back, so repeated configure passes stop churning the dma32
  window into fragments no 224 MiB section fits: measured 2026-10-09 on
  boot 2105990f, a 222,980,416-byte `BO_INIT` failed in every process
  at `bo_total_bytes` 2.76 GB after the passes had run. New read-only
  `bo_pool_bytes`. The policy lives in `ane/t6021/ane_t6021_pool.h`
  with a host unit test (`tools/test_t6021_pool`).
- `libane`: the ABI-2 open allocates its six section BOs in descending
  size order, so the largest contiguous request lands while the
  window's largest hole is still fresh. The ioctl regression records
  the `BO_INIT` order and fails on the old order.
- `libane`: the ABI-2 (M2) client releases its six section BOs as soon as
  `PROG_LOAD` + `PROC_CREATE` succeed. On a first load the driver holds
  the section bytes anyway (`fw_ref`); on a dedup hit the client's copy
  is memory the firmware never reads. Before, every live `ane_nn` pinned
  a full duplicate of its program's sections inside the sub-4-GiB window
  until close, so a resident session failed `BO_INIT` for prog_006's
  222,980,416 B kernel section once the 38-program set was cached:
  measured 2026-10-09 on boot 2105990f, `REFUSE: prog_006` with
  `bo_total_bytes` at 2,757,607,424. The host regression
  (`tools/test_libane_ioctl`, "resident pattern" case) fails on the old
  libane and passes now.
- `ane_t6021`: `trace_td=1` now creates the debugfs blob in its own
  directory, `ane_t6021_trace/trace_td` (was `ane_t6021/trace_td`). The
  probe (`stats=1`, the default) owns the `ane_t6021` debugfs directory;
  the trace setter created the same name again, lost the collision
  (`debugfs: 'ane_t6021' already exists`), and the parameter flipped with
  no `trace_td` file to read (measured on the M2, 2026-10-07). The trace
  now owns its directory outright, so any load order works and neither
  side removes the other's files. Update readers of the old path.

## 0.4.6 (2026-10-06)

This release makes each H14 (T6021, M2 Max) CALL faster and adds the first
Ultra (two-die) plumbing. The CALL change is in `ane/t6021` only. On one
M2 Max boot (aurora 11.38) with the same sampler, the ledger ANE cell goes
from 2,932.5 to 3,348.0 jobs/s (+14.1 %) and the add median from 0.359 to
0.257 ms with `call_poll_us=1000` on top of `call_settle_us=0`. The Ultra
change also touches `ane.ko` and libane (see its entry). The M1 and M2
drivers keep ABI 1 and ABI 2. The earlier removal of the 1,000 us settle
(about 674 to about 3,206 jobs/s on the aurora 11.36 install, the
`call_settle_us` entry below) did not reproduce its absolute figure on
11.38: the same configuration measured 2,933 there. Quote the paired
numbers from one boot.

- `ane_t6021`: the CALL finish-event wait polls at a 1 us cadence for the
  first `call_poll_us` microseconds (default 1000), then falls back to the
  50-100 us sleep, so the 254 ms encoder does not spend its wait on timer
  wakeups. `call_poll_us=0` restores the old cadence at run time. A 300 us
  busy-spin head added +0.4 % for about one core and is not included.
  Receipt: `receipts/2026-10-06-t6021-call-poll/README.md`.

- Ultra die-1 plumbing (#120, static: no T6002 or T6022 hardware run).
  `ane.ko` takes the SET base from the node's set `reg` window and
  qualifies (compatible, SET base) pairs through a die-keyed table; T6002
  die 1 is recognized behind `allow_unqualified`, and an unknown base
  refuses. `ane.ko` names its stats debugfs directory per device, and die 0
  keeps the legacy name. `ane_t6021` keys the T6022 per-die data on the same
  window. `omarchy-ane-firmware-fetch` refuses on a die-1 node (no pin).
  New opt-in overlays `t6002-ane-die1` and `t6022-ane-die1`. libane:
  `ane_m2_init_ports()` takes a new `dev_id` argument, and `ane-run` gains
  `--dev` / `ANE_DEVICE`. Callers of `ane_m2_init_ports()` must pass the
  device id (0 for the first accel node).

- `ane_t6021`: the Makefile searches `ane/src` with `-iquote`, so a kernel
  header package that ships its own `include/uapi/drm/ane_accel.h`
  (linux-aurora-headers does) no longer shadows the in-tree UAPI header.
  Before the fix the out-of-tree `ane_t6021` build failed on stock aurora
  11.36 headers with `ANE_M2_MAX_BINDS` and `ANE_ABI_M2_MAJOR` undeclared.
  The M1-family module was not affected (#132).

- `ane_t6021`: a CALL returns on the firmware's IO_T2H finish event with
  no fixed sleep after it. `call_settle_us` now defaults to 0 (was 1000
  us per call): the finish event already marks the output in DRAM. H14
  add on the M2 Max drops from 1.478 to about 0.31 ms per call. A
  completion timeout now logs at error level and quarantines as before.
  Receipt: `receipts/2026-10-06-t6021-call-settle/README.md`.

- `tools/wdt-hang-test`: an out-of-tree test module that arms the Apple SoC
  watchdog (WD1) through the kernel-registered `apple_wdt` device (DT-window
  fallback only), pets it from a kernel thread, then hangs the CPU with
  interrupts off, to prove the unattended reset-to-stock recovery under a
  one-shot boot (Limine `LoaderEntryOneShot`, GRUB `next_entry`). W=1 clean
  on the 7.1.13-3-1-ARCH and aurora trees; experimental and hardware-gated:
  it never autoloads and is not packaged. Receipt:
  `receipts/2026-10-04-wdt-hang-test/README.md`.

## 0.4.5 (2026-10-04)

This release moves the device tree overlays and the opt-in file to the
overlay contract of omarchy-mac-boot. The move breaks hand installs and
opt-in files from earlier releases. The release also adds the opt-in H15
(M3) bring-up module. It changes no code that DKMS builds and no libane
code: `git diff v0.4.4..v0.4.5 -- dkms.conf ane/Makefile ane/src
ane/include ane/ane_stats_show.c ane/t6021 libane` is empty, so `ane.ko`,
`ane_t6021.ko`, libane and the ioctl interface (ABI 1 and ABI 2) are the
bytes of v0.4.4. The new `ane/h15/` tree is not in `dkms.conf`, and the
package does not build it. The packaged drivers, libane and the firmware
fetch are unchanged, so this release has no new hardware run. The path move
is tested on the host only.

- Breaking: the overlays move to the overlay contract of omarchy-mac-boot
  (#124; omacom/omarchy-mac-pkgs#3, the port of omacom/omarchy-mac#677).
  They install to `/usr/lib/omarchy-mac-boot/dtb-overlays/PREFIX/`, the
  directory that omarchy-mac-boot ships for package-owned overlays. They
  were in `/usr/share/omarchy-platform/dtb-overlays`. The second `Target`
  of `90-omarchy-ane-dt.hook` is now
  `usr/lib/omarchy-mac-boot/dtb-overlays/*/*.dtbo`, the glob of the
  omarchy-mac-boot hook. A package upgrade moves the files. A hand install
  (`packaging/build-dtbo /`) must move: while `.dtbo` files are in
  `/usr/lib/omarchy-platform/dtb-overlays` or
  `/usr/share/omarchy-platform/dtb-overlays`, `omarchy-ane-dt apply`
  refuses, keeps the current copies, and names the steps.
- Breaking: the opt-in file is now
  `/etc/omarchy-mac-boot/dtb-overlays.opt-in`. It was
  `/etc/omarchy-platform/dtb-overlays.opt-in`. Re-add your opt-in lines to
  the new file. Nothing reads the old file, and the package does not copy
  its lines. `omarchy-ane-check`, the docs and the H15 and H16 runbooks
  name the new file (#124).
- On Arch Linux ARM installs without omarchy-mac-boot, the package creates
  the overlay directory, and `omarchy-ane-dt apply`, `update-m1n1-dtbs`
  and the two `90-omarchy-ane-dt` hooks work as before (#124).
- H15 (M3) opt-in experimental bring-up module (#121): `ane/h15/` with
  four SoC rows (T8122, T6030, T6031, T6034), a module with four stages
  (dt, status, wrapper, boot) and a host self test of its ADT walker
  (`make -C ane/h15 check`); three experimental overlays in `ane/h15/`
  (T8122, T6030, T6031; opt-in key `ane-h15-experimental`); three
  data-only overlays in `packaging/dt/`; and `tools/omarchy-ane-h15-stage`.
  The README data-only table marks the new overlays (#122). The module has
  no `MODULE_DEVICE_TABLE`, so nothing loads it automatically, and its
  probe refuses unless `optin=` names the SoC. No M3 has run it. Only
  stage 1 touches MMIO: it reads the PMGR power-state words and writes no
  register. Stages 2 and 3 refuse on H15 until a macOS capture clears the
  words that are INFERENCE now.
- H15 host-side fixes (#123): the module compiles (a macro that holds a
  list was used as an expression); the ADT walker matches child names,
  accepts nodes with no properties, reads segment-ranges as 64-bit fields
  and no longer reads an uninitialized error; the self test no longer hangs
  on property sizes that are not a multiple of 4; the probe refuses
  (`-ENXIO`) when `of_iomap` returns NULL; `ps_wait_ms` is a limit for each
  word, as documented; the compile line in `ane/h15/README-bringup.md`
  uses `-O dtb`. #123 reports a clean W=1 build against the aurora
  `ane-driver-aurora` tree. The T8122 overlay applies to all five T8122
  board device trees. New volunteer runbook: `docs/h15-volunteer.md`.

### Known limits

- No hardware ran this release. The code that DKMS builds and libane are
  the bytes of v0.4.4, whose hardware results are in
  `receipts/2026-10-03-omarchy-ane-0.4.4/README.md`. The path move is
  tested on the host only, with fake roots
  (`receipts/2026-10-04-omarchy-ane-0.4.5/README.md`). The release gates
  did not install it on a Mac.
- T6000 and T6020 stay on by default in the overlays table of this release.
  The Omarchy package (omacom/omarchy-pkgs#745) keeps them opt-in through
  its recipe until a passing row from the DKMS modules of this package
  exists. On these chips, the overlay and the DKMS modules of this package
  have not run: their rows ran the kernel's own ANE driver and device tree.

## 0.4.4 (2026-10-03)

This release fixes the packaging defects of v0.4.3 and changes no driver or
libane code: `git diff v0.4.3..v0.4.4 -- ane libane dkms.conf` is empty, so
`ane.ko`, `ane_t6021.ko`, libane and the ioctl interface (ABI 1 and ABI 2)
are the bytes of v0.4.3. The v0.4.3 tag sits on `0477f72`, the merge of the
T6000 promotion, one commit before its CHANGELOG move. So the v0.4.3 tarball
has no 0.4.3 section, and it carries a committed `__pycache__` file, stale
T6000/T6020 wording and a failing `test_promote_chip`. This release
supersedes it: its tarball carries the 0.4.3 and 0.4.4 notes.

- The promotion release step cuts the release it should (#117). The release
  notes cover every promotion merged since the last tag (verdict, chip, row
  shas, driver_source, merge sha), and the tag goes on the release commit,
  so the tarball carries its own CHANGELOG section, with `## Unreleased`
  kept as the first section. v0.4.3 tagged the T6000 merge before the
  CHANGELOG move and named only T6000. As merged in #117, the move left the
  entries under `## Unreleased` and wrote no version heading; this release
  writes the heading, and `test_promote_from_verdict` checks the moved text
  byte for byte, with an entry to move. Its fixture already held a
  `## 0.4.1` heading, so the old check passed without one. After a release,
  `## Unreleased` sits empty right above the released heading, and
  `promote_chip.py` keeps a blank line between a new entry and that heading
  (`test_promote_chip` checks it).
- The promotion flip is state-independent (#117). The check's case lists and
  the tested-header list take their order from rules (T8103, T6001 and T6021
  first, promoted chips sorted after them), not from seats in the v0.4.2
  five-chip list. Each `<chip>-ane.dts` carries one tool-written
  `Overlay state:` line, the offline suites take their chips from the
  overlays table, and `test_promote_chip` runs in the promotion gate. In
  v0.4.3 a T6002 promote and revert reordered the check's opt-in list, and
  the suite failed.
- `propose` stages only the tracked files that the flip wrote (#117).
  `git add -A` had shipped a committed `__pycache__` file in v0.4.3; it is
  removed, and `__pycache__/` and `*.pyc` are ignored now.
- The T6000/T6020 prose matches 0.4.3 (#117): the README's default-on line
  and the evidence cells name the passing rows 6eb94f49985b and
  3c9389040f51, the overlay dts headers and the libane receipt stop calling
  them untested or opt-in, and the libane entry and receipt carry the
  hardware numbers of #111.
- The 0.4.3 section below lists the rest of what v0.4.3 shipped: the probe
  change (#79), the T6020 firmware fetch at install, and the development-only
  changes (#108, #109).

### Research

- `docs/ultra-die1.md`: a design for the die-1 ANE of T6002 (M1 Ultra) and
  T6022 (M2 Ultra), from a read-only capture on one M1 Ultra under macOS and
  the ADT of both Ultra boards (`receipts/2026-10-03-ultra-die1`).
  Design only, no code: no Ultra runs Linux today.

### Known limits

- T6000 and T6020 are on by default from one passing community row each.
  Both rows ran the kernel's own ANE driver and device tree; on these chips
  the overlay and the DKMS modules of this package have not run.
- The hardware results of this release are in
  `receipts/2026-10-03-omarchy-ane-0.4.4/README.md`.

## 0.4.3 (2026-10-03)

- T6000 (M1 Pro) ANE on by default. (row 6eb94f49985b)
- T6020 (M2 Pro) ANE on by default. (row 3c9389040f51)
- A chip that is on by default and has a passing in-tree row gets the
  `aurora-dt` target, so its aurora-silicon/linux PR still comes after the
  overlay flip. The aurora tree decides: a disabled node is planned, an
  enabled node is a no-op, and no node is `SKIPPED`. Before, the target was
  lost once the overlay PROMOTE merged.
- The promotion gate computes the fresh verdict on the base branch. It ran
  on the PR head, where the chip is already flipped, so every PROMOTE PR
  failed (runs 37144741326 and 37146125714, PR #113). The gate now merges
  while the verdict and the targets stay the same, every judged row keeps
  its outcome, and no new judged row fails; new passing rows and rows that
  are not judged do not refuse a PR. It refuses to run on a checkout that
  already has the PR's flip.
- `propose` closes its own open promotion PR (labeled `auto-promotion`, from
  this repository) whose chip it no longer proposes, with a comment, so no
  stale PR reaches the gate.
- `promote_from_verdict.py aurora-plan` skips a chip that has no ANE node on
  any board of the aurora base branch, as `aurora-wip` before #155 merges.
  It prints a line that starts with `SKIPPED`, writes no plan entry and
  exits 0, so `aurora-pr` does not run and the promotion run stays green.
  Before, the run failed (promotion run 37143877823, T6000). A build error,
  a node on some boards only, or a node that the change cannot enable still
  fails the plan.
- libane (M1/ABI 1) loads the ANEC payload straight into the command buffer
  object and parses the task stream there: one pass instead of two and no
  458 MB staging buffer resident for the life of the network. `nn->data` is
  `NULL` on this path. `ANE_LOAD_STAGED=1` restores the old staged load for
  A/B. The direct path reports `model_header`, `model_map`, `model_map_copy`
  (or `model_pread_fallback`), and `model_zero_tail` stages. The ABI and every
  header are unchanged; the T6021 (ABI 2) loader is untouched. Verified on
  hardware before the merge (#111): on T8103 the whole-encoder cold open went
  from 178.3 to 107.9 ms `init_total` (182.4 to 101.1 ms on the repeat arm),
  `open_ms` 67-72 ms less; on T6001 from 361.4 to 286.5 ms, `open_ms` 828.5 to
  760.1 ms; encoder output bit-exact across arms and smoke 20/20 on both
  chips. Protocol and runs: `receipts/2026-10-03-libane-no-staging/README.md`.
- `omarchy-ane-probe` reads data-only tables whose sections or `reg` lists
  are wrapped as `{v, src}` leaves (#79). Before, a wrapped `reg` list failed
  the whole `soc_table` section.
- On T6020 the pacman hook fetches the ANE firmware at install and upgrade:
  `omarchy-ane-firmware-fetch` has `apple,t6020` in its default list.
- Development only, not installed by the package: every shell script runs
  with `set -euo pipefail`, the H13 program-load verifier is split and has a
  golden test (#108), and the pre-push hook finds `privacy_check.py` (#109).

## 0.4.2 (2026-10-03)

`ane` powers the ANE off when it is idle and maps each buffer object with one
DART TLB sync, both drivers log their register tracing at debug level, and
DKMS steps aside on a kernel that ships the ANE driver itself. This release
also adds a reachability verdict to `omarchy-ane-probe`, in-tree rows to the
promotion checker, a lab `dyn_pg` parameter to `ane_t6021`, and data and an
experimental bring-up module for chips after the M2. The ioctl interface
(ABI 1 and ABI 2) does not change: from `4f01bb3` (v0.4.1) to `dc174cd`, no
line of `ane/src/uapi/drm/ane_accel.h`, `libane/ane.h` or `libane/ane_m2.h`
changes.

### Added

- `omarchy-ane-probe` has a `reachability` section (`schema_version` 2)
  (#102). From `/proc/device-tree` and `/sys` only, it tells whether this
  machine's device tree gives the ANE to the OS: `reachable`,
  `owned-elsewhere` (the node has the ADT property `exclave-assigned`),
  `not-exposed` (no ANE node, a disabled node, or no `reg`, `iommus` or
  `power-domains`) or `unknown`, with the reason and, for each ANE node, the
  status, compatible, windows, IOMMU, power-domain and mailbox targets,
  `exclave*` properties and the bound driver. It also gives the `exclave`
  record of `data/ane-soc/<soc>.json` and the `adt` phram region. It reads
  no register. One run on a MacBook Neo or an M5 Mac answers the question.
- libane prints per-stage load times and per-`ane_exec` times on stderr when
  `ANE_TRACE_TIMING` is set (not empty, not `0`): one line
  `LIBANE: TIMING stage=NAME ms=MS bytes=N` per stage (#101). Unset, each
  stage costs one branch.
- `tools/ane_cold_start.py` measures the per-process program open of the
  resident worker (seal, fork, `dlopen`, `ane_init`) without a submit, under
  the ANE lock and the idle rule. `docs/ane-worker.md` designs a persistent
  worker (#101). See `receipts/2026-10-03-ane-cold-start/README.md`.
- `omarchy-ane-check` prints `dtbs_source` and `driver_source`, the fields
  the community collector records (#97). When `DTBS=` is set in
  `/etc/default/update-m1n1` (`dtbs_source=kernel`), it names no opt-in key:
  overlay opt-in has no effect there, and `omarchy-ane-dt apply` refuses.
- The promotion checker judges in-tree rows (`driver_source=intree`) like
  DKMS rows (#97). A PROMOTE with a passing in-tree row also has the target
  `aurora-dt`: `tools/aurora_dt.py` and `promote_from_verdict.py aurora-plan`
  and `aurora-pr` make the aurora-silicon/linux device-tree PR. The job that
  builds the aurora tree holds no credential; only the PR step, which builds
  nothing, gets the secret `AURORA_PR_TOKEN` (a dry run without it).
- `ane_t6021` has a lab parameter `dyn_pg` (default 0, T602x only) (#96).
  With `dyn_pg=1`, probe sends the selene firmware command
  `SET_DYNAMIC_POWERGATE` = 1 after CONFIG_GET, so that the firmware can turn
  the compute islands off between jobs. With `dyn_pg=1`, `trace_td` reads no
  TD word. The debugfs file `ane_t6021/ane_pg_state` prints the seven ANE
  power-state words. Measured on one M2 Max laptop: the firmware answers the
  command with status 0, but `ane_pg_state` shows the islands fully on
  (ACTUAL 0xf) before the first job, 30 s after the last one, and every 10 s
  through the idle blocks, so it does not gate them. Smoke, gates, encoder
  time and output bits are the same as with the defaults. With
  `dyn_pg=1 boot_prevent_nap=0`, the firmware boots but does not answer
  CONFIG_GET (-110), and `ane_t6021` registers no device. Design:
  `receipts/2026-10-03-t6021-dynpg/README.md`; measurements:
  `receipts/2026-10-03-t6021-v042-gate/README.md` (#106).
- `ane/h16`: an opt-in experimental bring-up module, `ane_h16`, for the M4
  family (T8132, T6040, T6041) (#100, #103). It is not part of the package
  or the DKMS build, nothing autoloads it, and it refuses to probe without
  `optin=SOC`. `stage=status` powers the ANE domains, waits until every pmgr
  ANE state word reads ACTUAL 0xf, and logs the CPU and mailbox registers.
  `stage=boot` also validates the iBoot-preloaded firmware against the
  pinned image, maps it at the ADT addresses, releases the ASC CPU and
  answers the RTKit start. It registers no DRM device and sends no ANE
  command. No silicon has run it. `data/ane-soc/t8132.json`, `t6040.json`
  and `t6041.json` carry the H16 values from the macOS 27.0 images, with
  T6040 as the M4 Pro and T6041 as the M4 Max. See
  `receipts/2026-10-03-ane-h16/README.md` and `ane/h16/README-bringup.md`.

### Changed

- `ane` powers the ANE off when it is idle (#95). Before, probe kept a
  runtime-PM reference until the driver unbound, so the ANE power domains
  (`ane_sys`, `ane_sys_cpu`, `ane_base`, `ane_set*`) and its three DARTs
  stayed on all the time. Now the device suspends `autosuspend_ms` after the
  last open, ioctl or close (module parameter, default 1500), and the next
  one powers it up again. `autosuspend_ms=0` keeps the old behavior. At run
  time, `power/autosuspend_delay_ms` on the ANE platform device changes the
  delay, and `echo on > power/control` keeps the ANE powered. A write to the
  `reset` attribute and a file close now hold the device powered while they
  run. 32-bit (compat) ioctls go through the same command filter and power
  reference as native ones. `ane_t6021` does not change. Measured on one M1
  laptop (T8103) and one M1 Max laptop (T6001), with `ane.ko` from the #95
  head `198db99`, before #104 changed the map path: with the ANE suspended,
  idle system power per 5-minute block equals the level with the module
  removed (T8103 within 33 mW, T6001 within 7 mW). With the ANE held on, it
  was 107 to 173 mW higher on T8103 and 2.10 to 7.65 W higher on T6001. The
  first open after 5 s of idle took a median of 330 µs on T8103 (10 µs when
  warm) and 0.35 ms on T6001. Whole-encoder outputs stayed bit-exact, smoke
  runs from the suspended state passed 20 of 20, and about 300 suspend and
  wake cycles per machine gave no error. Design:
  `receipts/2026-10-03-ane-autosuspend/README.md`; measurements:
  `receipts/2026-10-03-omarchy-ane-0.4.2/README.md`.
- DKMS skips a kernel that ships the ANE driver itself
  (`CONFIG_DRM_ACCEL_ANE=y` or `=m`, aurora-silicon/linux #155): that kernel
  keeps its own `ane.ko` and `ane_t6021.ko` (#94). `dkms.conf` sets
  `BUILD_EXCLUSIVE_CONFIG="!CONFIG_DRM_ACCEL_ANE"`, so `dkms build` exits 77
  (excluded) there. Before, `ane_t6021` failed to build on such a kernel:
  its `include/uapi/drm/ane_accel.h` hides ours and lacks
  `ANE_ABI_M2_MAJOR`. `tools/test_dkms_exclusive.py` runs dkms on both kinds
  of kernel.
- libane fills the program staging buffer with one `pread` and zeroes only
  the tail past a short read, instead of a `memset` over the whole buffer
  first (#101). The buffer stays 16 KiB aligned and zero past the file end.
- `ane` maps a buffer object into the ANE DARTs with one `iommu_map_sg`
  call, so apple-dart invalidates the TLB of each DART once per BO_INIT
  (#104). Before, BO_INIT called `iommu_map` once per 16 KiB page, and each
  call invalidated the TLB of all three DARTs: 83,865 invalidates for the
  458 MB Parakeet encoder program. A stray PTE in the new range is still
  cleared once and the map retried. Module parameter `map_batch` (0644,
  default 1); `echo 0 > /sys/module/ane/parameters/map_batch` selects the
  old per-page path for the next BO_INIT, without a reload. The unmap path
  does not change. Measured with the whole Parakeet encoder, 10 cold opens
  per arm: on one M1 laptop (T8103), BO_INIT took 22.77 ms instead of
  179.85 ms (median), and the whole open took 165 to 173 ms less; on one M1
  Max laptop (T6001), BO_INIT took 24.75 ms instead of 232.74 ms (minimum),
  and the whole open took 574.25 ms instead of 787.89 ms. Outputs stayed
  bit-exact, the job count exact, and the smoke passed 20 of 20. Stress runs
  of 10 minutes that flipped `map_batch` every 60 s gave no error. Design:
  `receipts/2026-10-03-ane-iommu-batch/README.md`; measurements:
  `receipts/2026-10-03-omarchy-ane-0.4.2/README.md`.

### Fixed

- `ane_stats`: a submission that opened a busy period while the previous
  period was closing could read the previous end too early, so `busy_ns`
  could count the overlap twice. The start of a busy period is now read
  after the submission wins the transition, so periods never overlap (#93).
  Both drivers.
- The M3 data files `data/ane-soc/t8122.json`, `t6030.json` and `t6031.json`
  no longer name the 0x4000 window at engine + 0x1050000 as the ANE mailbox,
  or give a reordered mailbox IRQ list (#98). No source gave either. That
  offset is RVBAR on T6021. Both values are now null, with the expected
  wrapper + 0x8000 address in the reason. Also corrected: the mailbox
  compatible in all four M3 files (the T6030 one contradicted its own
  source), the j575d board (firmware-list chip id 0x6032) and its three
  missing die-1 windows, the T8122 iommu-parent reason, and the driver
  family. See `receipts/2026-10-03-ane-h15`.
- `omarchy-ane-check` lists `sudo omarchy-ane-firmware-fetch` in the
  bring-up steps of every SoC that `ane_t6021` drives (#92). Before, a T6021
  that a promotion revert made opt-in again got no firmware step.
- `ane_t6021` logs its probe and firmware boot progress at debug and info
  level, not at emergency level (#94). Emergency lines go to every console
  and terminal at any loglevel, on every M2 Max boot. The `BOOT-PHASE`
  markers and the register readouts are debug messages now: turn them on
  with dynamic debug (for example `ane_t6021.dyndbg=+p` on the kernel
  command line). The outcome lines are info, and the dump when the ANE CPU
  starts but never reports READY is an error. aurora-silicon/linux #155 has
  the same change (Chris Kearney).
- `ane` and `ane_t6021` log register and address tracing at debug level
  (#94): the `ANERD`/`ANEWR` recovery trace, the `ps probe` SET-window
  reads, the `ANE-resume` progress lines, each DART, the ChMan table
  entries, RTKit endpoint starts and messages, and the firmware alias and
  staging addresses. Dynamic debug shows them (`ane.dyndbg=+p`,
  `ane_t6021.dyndbg=+p`). The only info lines left on the `ane` probe path
  are `DART containment armed` and `loaded ane`. Faults stay at error and
  warning level.
- `modinfo ane_t6021` shows one license and one description (#94). Each of
  the three objects of the module carried its own.
- The T6000, T6001 and T6002 overlays have no `ane_set5` power state at
  pmgr 0xc030 (#94). It is past the end of the ANE pmgr range in the ADT
  (0x28e080000+0xc02c), and the ane node does not use it.
- The T8103, T600x and T8112 overlays set `status = "okay"` on their ANE
  power states (#94). aurora-silicon/linux #155 has these nodes at the same
  paths, disabled where its ANE is disabled (T8103, T6000, T6002, T8112).
  On such a kernel `omarchy-ane-dt` refused the overlay ("power-domains
  names ..., which is disabled") and the ANE stayed off.
  `tools/test_ane_overlays.py` now also disables the power states when it
  checks the overlay over disabled kernel nodes.
- The host suites run from a `git archive` of the tree, as in the release
  tarball: `tools/test_promote_chip.py` and `tools/test_promote_from_verdict.py`
  no longer need `.git` (#92).

### Research and receipts

- H17 b0 (`receipts/2026-10-03-ane-h17`, #102): T8140 (MacBook Neo), T8142
  (M5) and T6050 (M5 Pro/Max) are the `ane_t6021` firmware-boot family, on
  the H16 window and DART layout plus the `exclave-*` properties. T8140 has
  the six M4 (T8132) windows with the same sizes; T8142 differs in one
  window size; T6050 has the seven-window shape of the M4 Max (T6041). The
  receipt compares the 27 per-SoC entries of `ane_t6021` with the H16 b0
  values (7 have no local value), names what the probe and the owner's
  macOS can supply, and plans b1 as H17 rows in `ane_h16`.
  `data/ane-soc/t6050.json` no longer says that the j775d `ane1` has no
  exclave marking: no source measured it.
- H18 b0 (`receipts/2026-10-03-ane-h18`, #99): T8152 (M6) is the `ane_t6021`
  firmware-boot model as a new `ascwrap-v8` variant, and T8150 (A19 Pro) is
  Exclave-owned. The receipt lists the 28 per-SoC entries a T8152 smoke
  module needs (13 have no local value), the macOS `ioreg` commands an M6
  owner can run to supply some of them, and the b1 module plan.
  `data/ane-soc/t8152.json` records the firmware `_rtk_patchbay` tags and
  load commands, the `ane1` clock-gates, and the new family statement. Its
  `compiler` leaf is gone: it cited the aurora DT and repeated
  `hwx_lab_cross_target`.
- The T6021 power-down study (`receipts/2026-10-03-t6021-powerdown`, #105):
  the state that a power-domain cycle loses, the firmware power commands, and
  ranked designs. Offline, no hardware.
- The T6021 gate of this release (`receipts/2026-10-03-t6021-v042-gate`,
  #106), on one M2 Max laptop with the `ane_t6021` of `5a457a3` (srcversion
  `59494CBC56F28ED8D1122C6`): at the default parameters the release gate
  passed (smoke, 9 gate ops, `ane_stats` items 1-4), and the whole encoder
  was bit-exact at 254.306 ms min-of-min. `dyn_pg=1` was accepted but did
  not gate, and `dyn_pg=1 boot_prevent_nap=0` was refused at probe
  (CONFIG_GET -110). On two boots with the driver never loaded, idle power
  was 14.46 to 14.50 W (5-minute block medians, fans off), against 14.69 to
  14.77 W with the driver bound: holding the driver costs 227 mW (95 %
  interval 207 to 246 mW, same session) to 275 mW (260 to 290 mW, against
  the gate run's blocks). With the driver never loaded, `ane_cpu` and
  `ane_sys` stay on and the other seven ANE power domains are off.

### Known limits

- Each on-by-default chip is tested on one machine.
- On Omarchy, the overlays apply only through an omarchy-mac-boot with device
  tree overlay support (omacom/omarchy-mac#677, not merged).
- `dyn_pg=1` does not gate the compute islands on the tested M2 Max firmware,
  and `dyn_pg=1 boot_prevent_nap=0` makes probe fail. Keep the defaults.
- `ane_t6021` keeps the ANE powered while it is bound; only `ane` suspends.
  On one M2 Max laptop that costs about 0.23 to 0.27 W at idle against a
  boot where the driver never loads (#106).
- Closing a program still unmaps its buffer objects page by page: the
  release of the whole encoder program took about 178 ms on T8103 and
  241 ms on T6001, the same with and without `map_batch`.
- No silicon has run `ane_h16`.
- The hardware results of this release are in
  `receipts/2026-10-03-omarchy-ane-0.4.2/README.md` and
  `receipts/2026-10-03-t6021-v042-gate/README.md`.

## 0.4.1 (2026-10-02)

Both drivers count ANE work for monitoring, and the M1 family has its own
smoke program. This release also adds a read-only ANE state probe, a
promotion flow that runs with no person in the loop, and cited ANE data for
twelve chips after the M2 that no driver binds. The ioctl interface (ABI 1
and ABI 2) does not change.

### Added

- `ane` and `ane_t6021` count ANE work (#57, #59, #63, #82). The sysfs file
  `/sys/class/accel/accel*/device/ane_stats` (mode 0444) gives `busy_ns` and
  `jobs`. `busy_ns` is the union of the submit-to-completion windows since
  the device bound, and it increases while work runs. `jobs` counts engine
  submissions: one per `ANE_SUBMIT` on `ane`, and one per firmware procedure
  call on `ane_t6021`. Program loads and other control messages do not count.
  The debugfs file `ane_timeline` (in `ane/` or `ane_t6021/`) lists the last
  256 submissions. The module parameter `stats` (default 1) controls both
  files. With `stats=0`, the driver creates neither file and the hot path is
  one branch. `tools/test_ane_stats` checks the counter logic on the host and
  runs in `make -C tools check`.
- `omarchy-ane-smoke` runs on the M1 family (#60, #62). T8103, T6000, T6001
  and T6002 run the H13 add program `fixtures/h13-anec/add/program-0.anec`.
  T6020, T6021, T6022 and T8112 run the H14 add program, as before. The H13
  check reads 64 fp16 planes and requires zero padding in the rest of the
  16 KiB output. See `receipts/2026-10-02-h13-smoke/README.md`.
- `omarchy-ane-probe` (`tools/omarchy-ane-probe`) prints the ANE state of an
  Apple Silicon Linux machine as one JSON document (#74). It reports the ANE,
  DART, power-domain and mailbox device-tree nodes (also of unknown
  generations), the `ane*` modules and interrupts, the runtime-PM state, the
  pacman versions, the DKMS module files, and the results of
  `omarchy-ane-check` and `omarchy-ane-firmware-fetch --check`. It compares
  the device tree with `data/ane-soc/SOC.json` when that file exists. It is
  read-only, needs no root and no network, exits 0, and keeps the document
  within 8 KiB. See `docs/ane-probe.md`.
- Data-only SoCs (#78, #85). `data/ane-soc/SOC.json` keeps cited ANE data for
  a chip that no driver binds, and `tools/validate_ane_soc.py` checks the
  file. `packaging/build-dtbo` installs each file as
  `/usr/share/omarchy-ane/soc/SOC.json`. It compiles a data-only overlay
  (`packaging/dt/SOC-ane-dataonly.dts`, root property `omarchy,data-only`) as
  a check, and never installs it. `omarchy-ane-dt` never applies one.
  `omarchy-ane-dt status` prints `data-only (no driver): SOC`, and
  `omarchy-ane-check` prints `DATA-ONLY SoC: SOC (no driver yet)`.
  `tools/gen_coverage_table.py` writes the README data-only table. CI: the
  `ane-soc-data` job, the data-only overlays in `tools/test_ane_overlays.py`,
  and the `aurora-dtbs` workflow for the M3 and later board device trees. See
  `docs/ane-soc-data.md`.
- Data files for twelve chips that no driver binds (#87): H15 (T8122, T6030,
  T6031, T6034), H16 (T8132, T6040, T6041), H17 (T8140, T8142, T6050) and
  H18 (T8150, T8152). `packaging/build-dtbo` installs the twelve files to
  `/usr/share/omarchy-ane/soc/`, where `omarchy-ane-probe` compares them with
  the device tree. Data-only overlays exist for T8132, T6040, T6041, T8140
  and T8152; `build-dtbo` compiles them and installs none. Each generation
  has a receipt (`receipts/2026-10-02-ane-gen-h15`, `-h16`, `-h18`,
  `receipts/2026-10-02-neo-ane`). The README data-only table gives the SoC,
  the internal name, the board count and the overlay state, and the README
  names the four ANE shapes after the M2.
- `tools/promote_chip.py` moves a chip between opt-in and on by default
  (#65). It edits the files that the earlier promotion PRs edited by hand:
  the overlays table row and tested header, the overlay's opt-in marker, the
  `omarchy-ane-check` chip lists, the `omarchy-ane-dt` opt-in list,
  `DEFAULT_ON` in `omarchy-ane-firmware-fetch`, the README chip table, and
  one CHANGELOG line. `--check` prints the diff and writes nothing. A second
  `--apply` changes nothing, and the reverse flip restores each file byte for
  byte, except the CHANGELOG line.
- The `promotion` workflow runs the promotion flow with no person in the loop
  (#66-#69, #71, #72, #75-#77). A daily cron judges the community rows and
  opens or updates one auto-promotion PR for each `PROMOTE` or `REVERT`
  verdict. The gate runs `make -C tools check` on the unchanged tree, the
  host tests on the PR, and a fresh verdict. Then it squash-merges the PR and
  cuts the patch release. `dry_run` prints the plan. The workflow refuses
  synthetic verdict fixtures in this repository.
- `make -C tools check` also runs `tools/test_libane_ioctl` (#55). It builds
  libane with `-ftrivial-auto-var-init=pattern` against a fake DRM node. The
  fake node refuses a nonzero pad, flags or reserved member, as the in-tree
  drivers do. No device runs this check.

### Changed

- Promotion (#61): one passing community row promotes an opt-in chip. The
  0.4.0 thresholds (3 rows from 3 machines, 2 owners, 2 boards and 2 kernel
  releases, and the uptime rule) are gone. Passing and failing rows for the
  same chip give `CONFLICT`, and the chip does not promote. A chip that is on
  by default goes back to opt-in when its latest judged row is not clean.
  Rows from a machine without omarchy-ane, and clean rows with no smoke
  attempt, are not judged.
- `omarchy-ane-check` prints the bring-up steps after its `UNTESTED SoC`
  line (#64): add the opt-in key, run `omarchy-ane-firmware-fetch` first on
  T6020, T6022 and T8112, then `omarchy-ane-dt apply`, `update-m1n1` and a
  reboot, and submit a collector row with `--ane-smoke` when the machine is
  idle. The lines that scripts read do not change.
- `omarchy-ane-firmware-fetch` (the install, `--check` and `--hook`) and
  `omarchy-ane-check` accept the Asahi vendor firmware copy (#56). The kernel
  loads `/usr/lib/firmware/vendor/apple/ane/NAME` before
  `/usr/lib/firmware/apple/ane/NAME`. A vendor copy that matches the pin is
  enough, and the tool fetches nothing. A vendor copy that does not match
  shadows the fetched copy, so the driver refuses the firmware. Then
  `--check` and `omarchy-ane-check` fail, the install exits 1, and `--hook`
  prints a note. With no matching vendor copy, the tool installs to
  `/usr/lib/firmware/apple/ane/`, as before.
- The DKMS source tree also needs `ane/ane_stats_show.c` and `ane/include/`.
  Both modules compile the shared stats code from there. A package recipe
  must copy them next to `ane/Makefile`, `ane/src/` and `ane/t6021/`.
- The host tests read the on-by-default chips from the overlays table and
  from `DEFAULT_ON`, so a promotion needs no test edits (#65, #70, #73, #84).

### Fixed

- libane sets the `pad` member of `struct drm_ane_bo_free` and of each
  `struct drm_ane_generic_bind` to zero (#55). Before, these members held
  stack bytes. The in-tree `ane_t6021` driver refuses a nonzero pad with
  `-EINVAL`, so ABI 2 `PROG_LOAD` could fail, and `BO_FREE` could keep the BO
  until the fd closed. The DKMS drivers of 0.4.0 accept the zero pads.
- `tools/promotion_check.py` counts a fault line only when it names the ANE
  device, its DARTs or its mailbox (#61). Before, an apple-dcp RTKit line
  gave a false T6001 `REVERT`, and rows from machines without omarchy-ane
  counted as T6000 and T8112 failures.
- `CHANGELOG.md` has the 0.1.0 to 0.4.0 sections again. #57 removed them.

### Research and receipts

- The T6021 default-on gate: boots A (reserved mode), B (own memory), P (the
  packaged boot.bin with the stock m1n1 1.6.1) and a final boot all passed 16
  of 16 gates, with the encoder bit-exact (#53).
- The 19 macOS words of each bulk DART, written at DART reset in the macOS
  order, give the same fault as the write on a live DART: `NO PGD FOR IOVA`
  at the first CALL. DART 0x20c with the reference value does not work with the
  Linux page tables in any write order (findings section 33, #54).
- The U-Boot `mtpkbd` candidate patches point at the
  joshuaswarren/aurora-u-boot branches (#58).

### Known limits

- The DART 0x20c write at DART reset, open in 0.4.0, is rejected (#54). One
  DART test stays open: 0x20c together with the DVA window and BOs above
  4 GiB. The cause of the encoder time on the M2 (254 ms under Linux, 89 ms
  under macOS) is still unknown.
- No driver binds the twelve data-only chips. With the package installed,
  `omarchy-ane-check` prints `DATA-ONLY SoC` on them, and `omarchy-ane-dt`
  applies no overlay for them.
- Hardware gates: T8103 and T6001 passed the `ane_stats` acceptance and the
  H13 smoke (20 of 20 calls bit-exact) on `cbb1beb`, with `ane.ko` built
  from source, not from the package. From `cbb1beb` to this release, no file
  under `ane/`, `libane/`, `fixtures/` or `packaging/omarchy-ane-smoke`
  changes. The T6021 result is in the GitHub release notes.

## 0.4.0 (2026-10-01)

The ANE is on by default on M1 (T8103), M1 Max (T6001) and M2 Max (T6021).
On T6021 the driver starts the ANE firmware from its own memory and replays
iBoot's runtime patches, so it needs no reserved-memory node from m1n1. This
is verified with the packaged m1n1 1.6.1 on one M2 Max laptop. The U-Boot
serial-stdin overlay stays an opt-in. This release also runs whole
models on the M2, adds ANE overlays with cited values for every M1 and M2 SoC
(the untested ones behind opt-in keys), and gives `ane_t6021` the package
version.

### Added

- The whole Parakeet TDT 0.6B v3 encoder runs on the M2 ANE as one
  Apple-compiled H14 program and one CALL. The output is bit-exact with the
  golden macOS capture, and the decode gives the golden 104 tokens (#17).
- `tools/hwx_h14_staged_to_anec.py` converts a multi-port HWX to an ANEC (#17).
- The Qwen M2 conformance harness compares each of the 38 programs with the M1
  step dump. All 38 programs conform, 456 of 456 runs (#12, #14).
- `tools/qwen_m2_decode.py` runs a greedy staged-Qwen decode on the M2 (#15).
- `tools/qwen_precision.py` measures where the M2 decode diverges and
  pre-registers the gate for a native macOS reference run (#20).
- `ane_t6021.bo_total_max_mb` (default 12288) sets the cap on the BO bytes
  held at one time. `bo_total_bytes` shows the bytes held now (#13).
- `ane_t6021.trace_td` records a read-only per-CALL timeline in debugfs. The
  default is 0, and then the CALL path does not change (#18).
- `packaging/dt/t6021-uboot-serial-stdin.dts`: an opt-in overlay
  (`uboot-serial-stdin-t6021`) that lets one M2 Max boot from the internal disk
  (#16).
- ANE overlays for every M1 and M2 SoC with cited values. T6000 (M1 Pro) and
  the T6002 (M1 Ultra) die 0 get the T6001 nodes, behind the opt-in keys
  `ane-t6000` and `ane-t6002`; `ane.ko` binds them with the `apple,t6000-ane`
  data. T6020 (M2 Pro) and the T6022 (M2 Ultra) die 0 get the T6021 nodes,
  behind `ane-t6020` and `ane-t6022`. The macOS 13.5 ADTs give the same values
  on these SoCs (#28). M3 and later get no overlay.
- `ane_t6021` binds `apple,t6020-ane` and `apple,t6022-ane` (`32b916c`) and
  `apple,t8112-ane` (`e91d350`, #47) as UNTESTED opt-ins. T8112 (M2) runs the
  13.5 `h14_ane_fw_bia_j4xx` image. `packaging/dt/t8112-ane.dts` (key
  `ane-t8112`) has the ANE, its three DARTs, its seven power states, the
  mailbox and the eFuse window; the chip revision and the ASC tunables come
  from the macOS 26.6.2 iBoot (#39, #47).
- `omarchy-ane-firmware-fetch` installs the T8112 image (pin `af587dfa…`), and
  has `--check` (reads only the installed file) and `--hook`.
  `omarchy-ane-check` checks the firmware on the M2 family with it (#43, #44).
- The pacman hook `90-omarchy-ane-firmware.hook` fetches the firmware at
  install and upgrade on T6021. Without network it prints a note, and the ANE
  stays off until a fetch succeeds and the Mac reboots (#44).
- `tools/t8112-kit`: a read-only kit that collects the T8112 iBoot ANE values
  under macOS and over the m1n1 proxy. A replay on 17 T6021 captures matches
  the T6021 data (#43).
- README "Promotion" and `tools/promotion_check.py`: an untested SoC goes on
  by default after 3 passing community rows from 3 machines, 2 owners,
  2 boards and 2 kernel releases, with no failing row. A row's smoke golden is
  the add fixture's, so T8112 rows can pass; the Parakeet encoder hash stays a
  developer check. The regression rule: a chip that
  is on by default goes back to opt-in when its latest row shows
  `omarchy-ane-check` not ready or a fault line, until a clean row lands
  (`promotion_check.py` prints `REVERT`). The community rows agree with every
  ANE and DART address, interrupt and power-domain path of the T6000, T6002,
  T6020 and T6022 overlays that they carry (#45, #48, #50).
- `omarchy-ane-smoke` runs the shipped H14 add program
  (`fixtures/h14-anec/add`) 20 times through `omarchy-ane-run`, one process
  per call, and prints one JSON line for the community collector. Exit 0: 20
  calls bit-exact against the exact fp16 sum of fixed inputs. Exit 1: a call
  failed, was not exact, or ran past 60 s. Exit 2: no smoke on this Mac. The
  M1 family gets exit 2, because no H13 add program has run through `ane-run`
  on a device. `omarchy-ane-check --smoke` runs it after the checks (#50).
- `omarchy-ane-check` prints `UNTESTED SoC: SOC` on every SoC other than T8103,
  T6001 and T6021, and fails on a SoC that no driver supports (#28).
- `tools/test_ane_overlays.py` applies every overlay to every linux-asahi
  7.1.13 board device tree it selects. `tools/asahi-dtbs` builds those device
  trees, byte for byte as the Arch package has them, and a dtc 1.7.2 with
  libfdt linked in. The `dt-overlays` workflow runs them (#28).
- `tools/qwen_m2_decode.py --m-per-prompt` decodes each prompt at
  M = len(prompt) + 32, the attention size that the M1 reference used (#27).
- `tools/native-macos/` runs the staged Qwen programs and the Parakeet encoder
  on a Mac's own ANE compile under macOS. `tools/staged-qwen/dump_step_ports.py`
  and `check_step_dump.py` dump and check every port of a decode step (#29).
- Research only, not in the DKMS build:
  - `ane/h13/ane_t8103_fw.c` stages 1-3 and the H13 `sCSneCmdProgramLoad`
    packer `tools/h13_progload.py`, for a Linux-side firmware start on the M1
    (T8103).
  - `ane/t6021/probes/ane_afbridge_probe.c` reads the 26 ANE0 AXI2AF bridge
    registers that macOS programs. Under Linux 0 of the 26 hold the macOS
    values (`17e3bdc`; #25, #33).
  - `ane/t6021/probes/ane_dart_probe.c` reads the ANE DART tunable and PERF
    words and can apply the bulk-DART tunables by group in the macOS order
    (`3ec8a28`, `61637ff`; #36, #37).
  - A prepared apple-dart kernel patch that writes the bulk-DART tunables at
    DART reset, and its boot record (#38, #41, #42, #46).
  - Candidate U-Boot `mtpkbd` patches in
    `receipts/2026-10-01-t6021-disk-boot/uboot-mtp/patches/`, not applied
    (#19).
  - Receipts for the ANE device-tree nodes and bindings on
    aurora-silicon/linux #65 (#22, #26).

### Changed

- The ANE is on by default on T6021: its overlay is enabled (no `ane-t6021`
  key), and the modprobe gate `install ane_t6021 /bin/false` is gone. The
  untested SoCs keep their opt-in keys (#44).
- `ane_t6021` runs the T6021 firmware from its own memory with iBoot's runtime
  patches replayed (`fw_alias_reserved=0`, the default). The reserved mode
  (`fw_alias_reserved=1`) needs the lab m1n1, and `ane_t6021` refuses it at
  probe, before any power access, unless no-map `/reserved-memory` nodes cover
  both iBoot firmware windows (`32b916c`, #44, #49).
- `ane_t6021` takes `ANE_VERSION` like `ane`. `dkms.conf` passes the package
  version, so `modinfo ane_t6021` and `/sys/module/ane_t6021/version` show it.
  Before, a DKMS build showed `unknown` or the commit of an enclosing repository.
- Port tables size every port from the HWX slot table (#10).
- `ane/t6021/gate/gate.sh` checks every island op with `tools/island_ref.py`
  (#13).
- `packaging/build-dtbo` installs `PREFIX-NAME.dts` as
  `PREFIX/omarchy-NAME.dtbo`, so one prefix can hold two overlays (#16).
- The overlays install to `/usr/share/omarchy-platform/dtb-overlays`, the
  directory of omacom/omarchy-mac#677. It was
  `/usr/lib/omarchy-platform/dtb-overlays`. A package upgrade moves the
  files. A hand install in the old directory (`packaging/build-dtbo /`) must
  move: while `.dtbo` files are in the old directory, `omarchy-ane-dt apply`
  refuses, keeps the current copies, and names the steps. `OVERLAY_DIR` in
  `omarchy-ane-dt` is the one place that names the directory.
  `update-m1n1-dtbs` and the two `90-omarchy-ane-dt` hooks stay: Arch Linux
  ARM installs have no omarchy-mac-boot (#28).
- The T600x and T602x overlays share their nodes through `t600x-ane.dtsi` and
  `t602x-ane.dtsi`. The T8103, T6001 and T6021 `.dtbo` files do not change
  (#28).
- `omarchy-ane-dt` names the libfdt trap when fdtoverlay renumbers a phandle
  (#28).
- The README chip table gives each SoC its ANE firmware, support state,
  overlay gate, and the data that is missing (#28).
- Removed: `omarchy-ane-m2-enable` and `packaging/modprobe/ane_t6021.conf`
  (#44).
- Removed: the `af_bridge_macos` experiment parameter. The 26 macOS AXI2AF
  bridge values did not change the encoder time (254.274 vs 254.276 ms) (#33,
  #34).

### Fixed

- libane: M2 send and read use the port model, not the ANEC header count.
  Before, `ane-run --ports` read only the first output, and 37 of the 38 Qwen
  programs failed on the M2 (#14).
- libane: the port-table build takes any task count from the header. Before,
  it refused programs with more than 128 tasks (#17).
- libane refuses a port-table ANEC whose `taskCount` is larger than its task
  stream (#17).
- `hwx_ports.py` and the converter read LC 0x40 tensor names longer than
  8 bytes (#17).
- `omarchy-ane-dt` skips each overlay on its own. Before, it skipped every
  overlay when the kernel tree had the ANE node (#16).
- `omarchy-ane-dt` counts only an enabled kernel node (no `status`, `"okay"`
  or `"ok"`) as the kernel's ANE node. Before, a disabled node, as in
  aurora-silicon/linux #65, made it skip the T6021 overlay, and the ANE stayed
  disabled. Now the overlay applies over the disabled nodes and enables them.
  The T6021 overlay sets `status = "okay"` on its three DARTs, so a merged
  DART does not stay disabled. `validate` refuses a new or changed node that
  names a disabled provider, and `omarchy-ane-dt status` ignores disabled ANE
  nodes. See `receipts/2026-10-01-ane-dt-disabled-nodes` (#23).
- Removed the CoreSight PC-sampling path of the research module
  `ane_t8103_fw`, after it caused a hard reset of the M1.
- T6020 uses chip revision 0x01 and its own iBoot ASC tunables. Before, it ran
  with the T6021 revision (0x11) and tunables, which differ in 13 of 24 values.
  T6022 keeps the T6021 values (`e91d350`; #45, #47).

### Known limits

- Each on-by-default chip is tested on one machine: one M1 (T8103), one
  M1 Max (T6001) and one M2 Max (T6021).
- T6021 disk boot: verified with the packaged m1n1 1.6.1 on one M2 Max laptop
  (with the opt-in U-Boot stdin overlay for that laptop's phantom keyboard
  input). The own-memory default needs no reserved-memory node.
- On Omarchy, the overlays apply only through an omarchy-mac-boot with device
  tree overlay support (omacom/omarchy-mac#677, not merged). With an older
  omarchy-mac-boot, `omarchy-ane-dt` refuses, and the ANE node must come from
  the kernel.
- T6000, T6002, T6020, T6022 and T8112 are untested opt-ins. Each applies only
  with its own key, and `omarchy-ane-check` prints `UNTESTED SoC: SOC` on it.
  On T6020, T6022 and T8112, run `omarchy-ane-firmware-fetch` by hand. Their
  overlays were checked against the linux-asahi 7.1.13 board device trees, not
  on hardware.
- Speed: the whole Parakeet encoder takes 254 ms per CALL on the M2 under
  Linux and about 139 ms on the M1 (T8103). On the same M2 under macOS, the
  same MIL takes 89 ms with the same output bits (#29). The cause is under
  investigation. Rejected causes, each with a receipt commit: the compiled
  program (the M2's native program also takes 253 ms under Linux; `3b6fcf4`,
  #32), the AXI2AF bridge tunables (`17e3bdc`, #33), the bulk-DART tunables
  0x220, 0x224 and the SID words (`ec6ab5a`, #37), and the ten Linux-only P-1
  writes (`ee469e2`, #40). Writing DART 0x20c on a live DART breaks
  translation (`ec6ab5a`); a write at DART reset is still open (`a332e6a`,
  `d5a0e74`).
- Qwen: all 38 programs conform, but STAGED-QWEN-REF passes 3 of 10 prompts
  against the M1 reference. The M2's own macOS compile and runtime give the
  same tokens as Linux on 10 of 10 prompts, with a logit difference of 0, so
  the 3 of 10 comes from H14 against H13 numerics, not from the driver (#29).
- `trace_td` is off by default. Use it for measurement only.
- The T8103 firmware start is research. The package does not contain it.
- The disabled-node kernel case that #23 fixes has not been booted.
- After the firmware starts, `ane_t6021` cannot unload. Only a reboot removes
  it.
- Hardware gates of this release: T8103 passed on 2298beb (package build,
  install, load, bind, `omarchy-ane-check`, ABI probe; the encoder smoke did
  not run), and `ane.ko` did not change after 2298beb. T6001 passed on
  be6b352: `ane.ko` is byte-identical to the earlier builds, the smoke tests
  pass, and `ane_t6021` cannot autoload on T6001. T6021 passed two disk boots
  on 73da8f8 with the lab m1n1, in the reserved mode and in the own-memory
  mode (#49): the device gate passed, Qwen program 20 matched the M1 golden
  (rel L2 0.00117), and the whole encoder was bit-exact at a 254.5 ms median.
  A third disk boot ran b6ef8f1 with the packaged m1n1 1.6.1 and the default
  own-memory mode: 16 gates passed, and the whole encoder was bit-exact at a
  254.3 ms median, with outputs byte-identical to the own-memory lab boot.

## 0.3.0 (2026-09-30)

- T6021 (M2 Max) opt-in driver `ane_t6021` on the stock kernel: DRM ABI 2
  (UAPI `23b8eef`, driver `27e996a`) and the libane ABI-2 backend (`8a4379e`).
- Complete T6021 device-tree overlay with the send-empty IRQ. The poll-TX
  kernel patch is not necessary.
- `hello_wait_ms` defaults to 0. This stops a mailbox IRQ storm of 700,000 per
  second and the 95-152 ms add stalls (#8).
- A recycle pool for the io BOs that the firmware saw. Before, the driver
  stopped after about 14,500 processes per boot (#9).
- A CALL completes on the firmware finish event. Long programs no longer
  return zero output (#11).
- Port-table tools for Apple-compiled H14 programs (`ane-run --ports`).

## 0.2.0

- T6001 recovery drains the retained tm/tq state after an engine wedge.
- `ane_boost` holds the CPU clusters at the top frequency while the engine
  works (`boost_idle_ms`, default 100 ms).
- The staged Qwen tool has a 512-token prefill mode. The M1 Qwen ANE cell
  passes the parity bar.

## 0.1.0

- First release: the `ane` DRM accelerator module, `libane` and the Python
  bindings, for M1 (T8103) and M1 Max (T6001), driver ABI 1 (`7529715`).
- A three-tier SoC gate: qualified, recognized-untested and unsupported.
