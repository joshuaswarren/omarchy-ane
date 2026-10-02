#!/bin/bash
# Non-destructive pull of the macOS out/ after each pass, hash-verified on arrival; `clean` removes
# the remote scratch at the end (python shutil.rmtree, never rm -rf). Every ssh runs under timeout.
#   TARGET=<macos alias> SCRATCH=<abs scratch on macOS> DEST=<local dir> bash fetch.sh [fetch|clean]
set -euo pipefail
TARGET=${TARGET:?} SCRATCH=${SCRATCH:?}
mac() { timeout 600 ssh -o ConnectTimeout=8 -o BatchMode=yes "$TARGET" "$@"; }
case "${1:-fetch}" in
fetch)
	DEST=${DEST:?}
	mkdir -p "$DEST"
	mac "cd '$SCRATCH' && find out -type f ! -name 'SHA256SUMS*' -exec shasum -a 256 {} + >out/SHA256SUMS.fetch && tar -czf - out" |
		tar -xzf - -C "$DEST"
	(cd "$DEST" && sha256sum -c out/SHA256SUMS.fetch --quiet)
	echo "fetched and verified: $(wc -l <"$DEST/out/SHA256SUMS.fetch") files -> $DEST/out"
	;;
clean)
	mac "python3 -c 'import shutil, sys; shutil.rmtree(sys.argv[1])' '$SCRATCH' && ls -d '$SCRATCH' 2>&1 || true"
	;;
*)
	echo "usage: fetch.sh [fetch|clean]" >&2
	exit 2
	;;
esac
