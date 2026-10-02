#!/bin/bash
# Stage the gap-window bundle to macOS over ssh, hash-verified on both ends; fetch the results back.
#   TARGET=<ssh alias of the M2 under macOS> SCRATCH=<absolute scratch path on macOS> \
#   STAGE=/var/tmp/gapwin-stage bash staging.sh
#   TARGET=... SCRATCH=... OUT=/var/tmp/gapwin-results bash staging.sh fetch
# No hostname is hardcoded: the alias comes from the operator's ssh config (see the runbook).
set -eu
TARGET=${TARGET:?ssh alias of the macOS host}
SCRATCH=${SCRATCH:?absolute scratch path on macOS}
STAGE=${STAGE:-/var/tmp/gapwin-stage}

case "${1:-push}" in
push)
  (cd "$STAGE" && sha256sum -c stage_manifest.sha256 --quiet)
  echo "local stage verified"
  ssh "$TARGET" "mkdir -p '$SCRATCH'"
  tar -C "$STAGE" -cf - . | ssh "$TARGET" "tar -xf - -C '$SCRATCH'"
  ssh "$TARGET" "cd '$SCRATCH' && shasum -a 256 -c stage_manifest.sha256" >"$STAGE/.verify.remote"
  bad=$(grep -cv ': OK$' "$STAGE/.verify.remote" || true)
  ok=$(grep -c ': OK$' "$STAGE/.verify.remote" || true)
  want=$(wc -l <"$STAGE/stage_manifest.sha256" | tr -d ' ')
  [ "$bad" = 0 ] && [ "$ok" = "$want" ] || { echo "REMOTE HASH MISMATCH ($ok/$want OK)"; exit 1; }
  echo "staged and verified: $ok files match on both ends"
  ;;
fetch)
  OUT=${OUT:?local output dir for the results}
  mkdir -p "$OUT"
  ssh "$TARGET" "cd '$SCRATCH' && find out -type f ! -name SHA256SUMS -exec shasum -a 256 {} + > out/SHA256SUMS && tar -czf - out" |
    tar -xzf - -C "$OUT"
  (cd "$OUT" && sha256sum -c out/SHA256SUMS --quiet)
  echo "fetched and verified: $(grep -c ': OK$' <(cd "$OUT" && sha256sum -c out/SHA256SUMS)) files"
  ssh "$TARGET" "rm -rf '$SCRATCH'"
  ;;
*)
  echo "usage: staging.sh [push|fetch]" >&2
  exit 2
  ;;
esac
