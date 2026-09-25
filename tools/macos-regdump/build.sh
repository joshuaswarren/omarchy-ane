#!/bin/zsh
# Build ANERegDump.kext, the CLI, and the ANE workload on a Mac with Xcode.
# Usage: ./build.sh <outdir>
set -euo pipefail

SRC=${0:A:h}
OUT=${1:?usage: build.sh <outdir>}
SDK=$(xcrun --show-sdk-path)
K=$SDK/System/Library/Frameworks/Kernel.framework
KCC=(xcrun -sdk macosx clang -target arm64e-apple-macosx14.0 -mkernel
     -nostdlib -fno-exceptions -fno-rtti -Wall -Werror -Wno-sign-conversion
     -I"$SRC" -I"$K/Headers")

mkdir -p "$OUT/obj" "$OUT/ANERegDump.kext/Contents/MacOS"
cc -Wall -Werror -o "$OUT/obj/test_filter" "$SRC/test_filter.c"
"$OUT/obj/test_filter"

"${KCC[@]}" -c "$SRC/ANERegDump/ANERegDump.cpp" -o "$OUT/obj/ANERegDump.o"
"${KCC[@]}" -c "$SRC/ANERegDump/kmod_info.c" -o "$OUT/obj/kmod_info.o"
"${KCC[@]}" -Xlinker -kext -Xlinker -export_dynamic \
	-o "$OUT/ANERegDump.kext/Contents/MacOS/ANERegDump" \
	"$OUT/obj/kmod_info.o" "$OUT/obj/ANERegDump.o" \
	-L"$K" -lkmodc++ -lkmod -lcc_kext
cp "$SRC/ANERegDump/Info.plist" "$OUT/ANERegDump.kext/Contents/Info.plist"
codesign -s - --force "$OUT/ANERegDump.kext"

xcrun -sdk macosx clang -target arm64-apple-macosx14.0 -Wall -Werror \
	-o "$OUT/aneregdump" "$SRC/aneregdump.c" -framework IOKit
codesign -s - --force "$OUT/aneregdump"
xcrun swiftc -O -target arm64-apple-macosx14.0 \
	-o "$OUT/aneprobe" "$SRC/aneprobe.swift"
codesign -s - --force "$OUT/aneprobe"
cp "$SRC/capture.sh" "$OUT/capture.sh"
