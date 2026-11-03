#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# m6-ane-stage.sh: run ONE ane_h18 stage on the M6 (T8152), as root, and collect the log.
#   m6-ane-stage.sh <1|2|3> <boot-id prefix> [GO [state [slots [mbox [ctrl]]]]]
#   m6-ane-stage.sh 4 <boot-id prefix> GO [state [slots [wait_ms [dart [hello [scratch]]]]]]
#   m6-ane-stage.sh 5 <boot-id prefix> GO [state [slots [wait_ms [dart [hello [scratch [handshake [chan [dinst [premap [props [pmu [pwait [dyn [defset [mcsz]]]]]]]]]]]]]]]]   (chan 0 to 4; dinst = mask of DART instances 0 to 3 to read, premap = send PREMAP_BUFFER, props = send the five CH_PROPERTY_WRITE commands)
# Step 1 = stage dt (no MMIO), step 2 = stage psd (read-only PSD slots 30 and 31), step 3 = stage power with
# confirm_power=1 (requests PSD state <state> on the first <slots> slots, default 1 = slot 30 only, then reads CPU_STATUS,
# ASC_STATUS and IORVBAR; mbox=1 also reads the Postoffice status words of three candidate channels, ctrl=1 also reads CPU_CONTROL).
# Step 4 = stage boot: starts the ANE core (CPU_CONTROL.RUN), watches it for wait_ms (default 300), reads at most 8 mailbox messages,
# clears RUN again. It also reads the status word of the alias-bank mailbox channel before RUN and watches that channel for messages.
# dart=1 reads twelve words of the ANE DART before RUN (needs two slots); hello=1 answers the firmware's RTKit management HELLO
# (version 12) and endpoint map in the TX half of that channel (RX +0x00, TX +0x10), one paired 16 byte access per message, nothing else.
# hello=2 also starts the system endpoints (crash log and the like), answers the firmware's IOP power state acknowledgement with AP power state ON, and
# starts the application endpoints once the AP power state is acknowledged. hello=3 also runs the restartable RTKit shutdown (AP power state QUIESCED, IOP power state SLEEP) before RUN is cleared, hello=4 the OFF shutdown the board stopped with.
# Step 5 is step 4 with map_fw=1, for a DTB that has the ANE DART: it needs power-domains and iommus in the ANE node, attaches the ANE to
# its DART through the iommu API and maps the two preloaded firmware segments before RUN. The state and slots arguments are ignored there
# (the kernel's PSD driver owns the slots). Step 5 counts as step 4: it needs a step 3 PASS and runs once per boot.
# scratch=1 reads the eight SCRATCH words of the engine block (read only) before RUN, when AP power ON is acknowledged and before the shutdown.
# It needs GO, a step 3 PASS on this boot, and runs once per boot: reboot before anything else after it.
# handshake=1 (step 5 only, with scratch=1 and hello 3 or 4) runs the SCRATCH handshake once AP power ON is acknowledged: it allocates the IPC and HEAP
# surfaces and the init block, writes SCRATCH0, SCRATCH1 and the wake word, and waits for the firmware's table address. It WRITES the SCRATCH words.
# chan=1 (needs handshake=1) then checks the table, hands the host-to-firmware rings to the host, writes READY to SCRATCH3 and waits for the firmware to clear it.
# chan=2 also sends PRINT_ENABLE and CONFIG_GET on the IO ring (it WRITES ring slots and the doorbell page). chan=3 sends START and RESOURCE_INFO_GET as well.
# chan=4 also loads the program file $WORK/ane_h18_prog.bin (pinned by sha256 below, copied to /lib/firmware) and runs the program on the IO ring: LOAD_PROGRAM,
# CREATE_PROCESS, PROCEDURE_CALL, then TERMINATE_PROCESS and DESTROY_PROGRAM. The core runs the firmware's own handling of a program: a refusal is logged, a hang costs a reboot.
# All of them write; a hang or reset is possible.
# The state defaults to 1: the AppleT8152PMGR kext decode in the lab receipts reads "on" as request 1 (or 2) for these slots,
# and names 1 as the first experiment. 0 is the value proven for the ATC USB slot 71 and GPU slot 3, 2 is the GPU ASC value.
# Step 3 needs the literal third argument GO (the operator's go-ahead) and a step 2 PASS on this boot. It may run again on
# the same boot only after a step 3 PASS whose slots returned to state 6; a failed or refused-late step 3 needs a reboot.
# Order is enforced per boot: step 2 needs a step 1 PASS marker, step 3 a step 2 PASS marker, in the work directory.
# Refuses on: wrong kernel release, boot id not matching (at least 8 hex characters), missing ANE node, a module whose sha256
# is not the pinned one, a module already loaded, a reused run directory, and any of the variables below set in the
# environment (they are for test-stage-script.sh, which sets M6_ANE_TEST=1). Writes only under $WORK.
# The verdict is read only from RESULT lines that this run added to the kernel log, for the stage of this step.
# A step 3 PASS whose log has a "did not return to state 6" line exits 3: read it before the next step.
# A hang costs one reboot.
set -u

if [ "${M6_ANE_TEST:-}" != 1 ]; then
  for v in WORK KO KO_SHA256 PROG PROG_SHA256 FWDIR WANT_UNAME UNAME_R BOOT_ID_FILE DT_NODE INSMOD RMMOD DMESG LSMOD; do
    if [ -n "${!v+x}" ]; then echo "m6-ane-stage: REFUSED: $v is set in the environment" >&2; exit 2; fi
  done
fi

WORK=${WORK:-/var/tmp/m6-ane}
KO=${KO:-$WORK/ane_h18.ko}
KO_SHA256=${KO_SHA256:-4f42ab1a856267e3b637c4d45cc7cc5c4c34fe47f735de5b4029da771396df1b}
PROG=${PROG:-$WORK/ane_h18_prog.bin}
# PROG_SHA256 is a list: any one of the reviewed program files may be staged (a space separated list of sha256, each one a file that went through review)
PROG_SHA256=${PROG_SHA256:-0050490148431c2a787a1e424b8779f74dc666a1a451111606f92fc10144d59f 8408eb553c8f1017f91472405fe87c4c609ae7d94f41a26005b0ed2de6c2196a 5ce40ebefc2d5aac7987b01fea4dc2c2642805bdb7f1662541bfc6a1a4a165a9 aa2d445daf23162c9d86f2518d1e6ad1a277e4e6e012cf99b35c1046d4946b02 8068aee9b91cc336ad6d9f0c8dcc8710bbae580e0777b4b4350a626598727d51 230f4caa8c2134389c214f83014fe6e17abc6bc3294f7e0d8898c6ef4427b4f4 b014b30cd284a0ad4abc836ac10681e58237ad40c5fe5993386252919c010dd6 3719e5ee14d9d4ea9c0dab1655a8b84785bd73fc04126f1de620edb60ca56d24 b623517dfdc86df62255dfac4278b9e9b3d1f49e33d1af364301f6ba6f670e7d 8f2ff344b55500b131915461b481aeb7fbd267f602f3e60d6ba427174adb23e3 8b36520d17f586689714eb3d3643be592df8b502146d80dc86bfbc020af98f72 b85d3928ddfaa5a16d54e98a750238494d43a9c4b2b22a738a9b39944f8b0399}
prog_pinned() { case " $PROG_SHA256 " in *" $1 "*) return 0 ;; esac; return 1; }
FWDIR=${FWDIR:-/lib/firmware}
WANT_UNAME=${WANT_UNAME:-7.1.12-aurora-m6usb-ARCH}
UNAME_R=${UNAME_R:-$(uname -r)}
BOOT_ID_FILE=${BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
DT_NODE=${DT_NODE:-/proc/device-tree/soc/ane@419600000}
INSMOD=${INSMOD:-insmod}
RMMOD=${RMMOD:-rmmod}
DMESG=${DMESG:-dmesg}
LSMOD=${LSMOD:-lsmod}

step=${1:-}
fw=0
if [ "$step" = 5 ]; then fw=1; step=4; fi
bid=${2:-}
go=${3:-}
state=${4:-1}
slots=${5:-1}
mbox=${6:-0}
ctrl=${7:-0}
wait_ms=${6:-300}
dart=${7:-0}
hello=${8:-0}
scratch=${9:-0}
handshake=${10:-0}
chan=${11:-0}
dinst=${12:-0}
premap=${13:-0}
props=${14:-0}
pmu=${15:-0}
pwait=${16:-0}
dyn=${17:-0}
defset=${18:-0}
mcsz=${19:-0}
die() { echo "m6-ane-stage: REFUSED: $*" >&2; exit 2; }

case "$step" in 1|2|3|4) ;; *) die "usage: $0 <1|2|3|4> <boot-id prefix> [GO [state [slots [mbox [ctrl]]]]]" ;; esac
case "$state" in 0|1|2|3|4|5) ;; *) die "state must be 0 to 5" ;; esac
case "$slots" in 1|2) ;; *) die "slots must be 1 or 2" ;; esac
if [ "$step" = 4 ]; then
  case "$wait_ms" in *[!0-9]*|"") die "wait_ms must be a number" ;; esac
  [ "$wait_ms" -ge 10 ] && [ "$wait_ms" -le 2000 ] || die "wait_ms must be 10 to 2000"
  case "$scratch" in 0|1) ;; *) die "scratch must be 0 or 1" ;; esac
  case "$handshake" in 0|1) ;; *) die "handshake must be 0 or 1" ;; esac
  case "$chan" in 0|1|2|3|4) ;; *) die "chan must be 0 to 4" ;; esac
  case "$dinst" in [0-9]|1[0-5]) ;; *) die "dinst (DART instance mask) must be 0 to 15" ;; esac
  case "$premap" in 0|1) ;; *) die "premap must be 0 or 1" ;; esac
  case "$props" in 0|1|2) ;; *) die "props must be 0, 1 or 2" ;; esac
  case "$pmu$pwait" in 00|01|10|11) ;; *) die "pmu and pwait must be 0 or 1" ;; esac
  [ "$pmu" = 0 ] || [ "$chan" -ge 3 ] || die "pmu needs chan=3 or 4"
  case "$dyn" in 0|1|2) ;; *) die "dyn must be 0, 1 or 2" ;; esac
  [ "$dyn" = 0 ] || [ "$pmu" = 1 ] || die "dyn needs pmu=1"
  case "$defset$mcsz" in 00|01|10|11) ;; *) die "defset and mcsz must be 0 or 1" ;; esac
  [ "$defset" = 0 ] || [ "$props" != 0 ] || die "defset needs props 1 or 2"
  [ "$mcsz" = 0 ] || [ "$chan" -ge 3 ] || die "mcsz needs chan=3 or 4"
  [ "$dinst" = 0 ] || { [ "$chan" = 4 ] && [ "$dart" = 1 ]; } || die "dinst needs chan=4 and dart=1"
  [ "$premap" = 0 ] || [ "$chan" = 4 ] || die "premap needs chan=4"
  [ "$props" = 0 ] || [ "$chan" -ge 3 ] || die "props needs chan=3 or 4"
  [ "$chan" = 0 ] || [ "$handshake" = 1 ] || die "chan=1 or 2 needs handshake=1"
  if [ "$handshake" = 1 ]; then
    [ "$fw" = 1 ] && [ "$scratch" = 1 ] && { [ "$hello" = 3 ] || [ "$hello" = 4 ]; } || die "handshake=1 needs step 5, scratch=1 and hello 3 or 4"
  fi
  case "$dart$hello" in 00|01|02|03|04|10|11|12|13|14) ;; *) die "dart must be 0 or 1 and hello 0 to 4 (2 also starts the endpoints, 3 also runs the restartable RTKit shutdown, 4 the OFF shutdown)" ;; esac
  if [ "$fw" = 1 ]; then
    [ -e "$DT_NODE/iommus" ] && [ -e "$DT_NODE/power-domains" ] || die "step 5 needs a DTB with iommus and power-domains in the ANE node"
  fi
  [ "$dart" = 0 ] || [ "$slots" = 2 ] || die "dart=1 needs two slots"
else
  case "$mbox$ctrl" in 00|01|10|11) ;; *) die "mbox and ctrl must be 0 or 1" ;; esac
fi
[ "${#bid}" -ge 8 ] || die "boot id prefix of at least 8 hex characters required"
[ "$UNAME_R" = "$WANT_UNAME" ] || die "kernel release $UNAME_R, want $WANT_UNAME"
boot=$(tr -d '-' < "$BOOT_ID_FILE" | cut -c1-8)
case "$(tr -d '-' < "$BOOT_ID_FILE")" in "$bid"*) ;; *) die "boot id $boot does not start with $bid" ;; esac
[ -e "$DT_NODE/compatible" ] || die "no $DT_NODE: this DTB has no ANE node"
[ "$(tr -d '\0' < "$DT_NODE/compatible")" = "apple,t8152-ane" ] || die "ANE node compatible is not apple,t8152-ane"
[ -f "$KO" ] || die "no module at $KO"
[ "$(sha256sum "$KO" | cut -c1-64)" = "$KO_SHA256" ] || die "module sha256 is not the pinned $KO_SHA256"
if [ "$step" = 4 ] && [ "$chan" = 4 ]; then
  [ -f "$PROG" ] || die "no program file at $PROG"
  prog_pinned "$(sha256sum "$PROG" | cut -c1-64)" || die "program file sha256 is not one of the pinned $PROG_SHA256"
  [ -d "$FWDIR" ] && [ -w "$FWDIR" ] || die "$FWDIR is not a writable directory"
fi
if $LSMOD | grep -q '^ane_h18 '; then die "ane_h18 is already loaded; rmmod it and reboot if step 3 ran"; fi
mkdir -p "$WORK"
[ ! -e "$WORK/step4-run-$boot" ] || die "step 4 already ran on boot $boot: reboot before any ANE step, the core may still be running"
if [ "$step" = 2 ] && [ ! -e "$WORK/step1-pass-$boot" ]; then die "step 1 has not passed on boot $boot"; fi
if [ "$step" = 3 ]; then
  [ "$go" = GO ] || die "step 3 needs the third argument GO (the go-ahead on the PSD state)"
  [ -e "$WORK/step2-pass-$boot" ] || die "step 2 has not passed on boot $boot"
  [ ! -e "$WORK/step3-run-$boot" ] || die "step 3 already ran on boot $boot: reboot first, the slot state is unknown"
fi

if [ "$step" = 4 ]; then
  [ "$go" = GO ] || die "step 4 needs the third argument GO (the operator's go-ahead)"
  [ -e "$WORK/step3-pass-$boot" ] && [ ! -e "$WORK/step3-run-$boot" ] || die "step 3 has not passed on boot $boot, or a later step 3 did not finish cleanly"
  [ ! -e "$WORK/step4-run-$boot" ] || die "step 4 already ran on boot $boot: reboot first, the core may still be running"
fi

run="$WORK/run-$(date -u +%Y%m%dT%H%M%SZ)-p$$-step$step-boot$boot"
[ ! -e "$run" ] || die "run directory exists"
mkdir "$run"
{
  echo "step=$step boot=$boot uname=$UNAME_R ko_sha256=$KO_SHA256 date=$(date -u +%FT%TZ)"
  uname -v
} > "$run/header.txt"

case "$step" in
  1) args="optin=t8152 stage=dt" ;;
  2) args="optin=t8152 stage=psd" ;;
  3) args="optin=t8152 stage=power confirm_power=1 psd_state=$state psd_slots=$slots"
     [ "$mbox" = 1 ] && args="$args read_mbox=1"
     [ "$ctrl" = 1 ] && args="$args read_ctrl=1"
     rm -f "$WORK/step3-pass-$boot"; : > "$WORK/step3-run-$boot" ;;
  4) args="optin=t8152 stage=boot confirm_power=1 confirm_boot=1 psd_state=$state psd_slots=$slots boot_wait_ms=$wait_ms alias_mbox=1"
     [ "$dart" = 1 ] && args="$args read_dart=1"
     [ "$fw" = 1 ] && args="$args map_fw=1"
     [ "$scratch" = 1 ] && args="$args read_scratch=1"
     [ "$handshake" = 1 ] && args="$args handshake=1"
     [ "$chan" -ge 1 ] && args="$args hs_chan=1"
     [ "$chan" -ge 2 ] && args="$args hs_cmds=1"
     [ "$chan" -ge 3 ] && args="$args hs_start=1"
     [ "$chan" = 4 ] && args="$args hs_prog=1"
     [ "$dinst" = 0 ] || args="$args dart_inst=$dinst"
     [ "$premap" = 0 ] || args="$args hs_premap=1"
     [ "$props" = 0 ] || args="$args hs_props=$props"
     [ "$pmu" = 0 ] || args="$args hs_pmu=1"
     [ "$pwait" = 0 ] || args="$args hs_wait_ms=5000"
     [ "$dyn" = 0 ] || args="$args hs_dyn=$dyn"
     [ "$defset" = 0 ] || args="$args hs_defset=1"
     [ "$mcsz" = 0 ] || args="$args hs_mcsz=1"
     [ "$hello" = 0 ] || args="$args rtkit_hello=1"
     [ "$hello" -ge 2 ] && args="$args rtkit_start=1"
     [ "$hello" = 3 ] && args="$args rtkit_stop=1"
     [ "$hello" = 4 ] && args="$args rtkit_stop=2"
     : > "$WORK/step4-run-$boot" ;;
esac
if [ "$step" = 4 ] && [ "$chan" = 4 ]; then
  cp "$PROG" "$FWDIR/ane_h18_prog.bin" && sync || die "cannot install the program file into $FWDIR"
  prog_pinned "$(sha256sum "$FWDIR/ane_h18_prog.bin" | cut -c1-64)" || die "the installed program file does not match the pin"
  echo "program file $(sha256sum "$FWDIR/ane_h18_prog.bin" | cut -c1-64) installed as $FWDIR/ane_h18_prog.bin" | tee -a "$run/header.txt"
fi
case "$step" in 1) want_stage=dt ;; 2) want_stage=psd ;; 3) want_stage=power ;; 4) want_stage=boot ;; esac
n0=$($DMESG 2>&1 | grep -c 'ane_h18 RESULT')
echo "insmod $KO $args (RESULT lines already in the kernel log: $n0)" | tee -a "$run/header.txt"
timeout 60 $INSMOD "$KO" $args > "$run/insmod.out" 2>&1
rc=$?
echo "insmod rc=$rc" | tee -a "$run/header.txt"
$DMESG 2>&1 | grep ane_h18 > "$run/dmesg-ane_h18.txt"
cat "$run/dmesg-ane_h18.txt"
timeout 60 $RMMOD ane_h18 > "$run/rmmod.out" 2>&1
echo "rmmod rc=$?" | tee -a "$run/header.txt"
$DMESG 2>&1 | grep ane_h18 > "$run/dmesg-ane_h18-after-rmmod.txt"
grep 'ane_h18 RESULT' "$run/dmesg-ane_h18.txt" | tail -n +$((n0 + 1)) > "$run/new-result-lines.txt"
verdict=$(grep -o "RESULT stage=$want_stage soc=t8152 verdict=[A-Z]* reason=[^ ]*" "$run/new-result-lines.txt" | tail -1)
echo "verdict: ${verdict:-none} (new RESULT lines: $(wc -l < "$run/new-result-lines.txt"), wanted stage $want_stage)" | tee -a "$run/header.txt"
if echo "$verdict" | grep -q 'verdict=PASS'; then
  if { [ "$step" = 3 ] || [ "$step" = 4 ]; } && grep -q 'did not return to state 6' "$run/dmesg-ane_h18.txt"; then
    echo "m6-ane-stage: step 3 PASS but a PSD slot did not return to state 6, log in $run" >&2
    exit 3
  fi
  if [ "$step" = 3 ]; then rm -f "$WORK/step3-run-$boot"; : > "$WORK/step3-pass-$boot"; elif [ "$step" = 4 ]; then echo "m6-ane-stage: reboot before any further ANE run on this machine"; else : > "$WORK/step$step-pass-$boot"; fi
  echo "m6-ane-stage: step $step PASS, log in $run"
  exit 0
fi
echo "m6-ane-stage: step $step did not pass (rc=$rc), log in $run" >&2
[ "$step" != 4 ] || echo "m6-ane-stage: after step 4 reboot before any further ANE run" >&2
exit 1
