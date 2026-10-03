# Persistent ANE worker: design

Status: design only. Nothing here is implemented. Measure first with
`tools/ane_cold_start.py` (see `receipts/2026-10-03-ane-cold-start/`).

## Problem

Each `mlx-omarchy-parakeet` process starts a private
`mlx-omarchy-ane-worker --serve`. On the M1 Max (T6001) that worker needs
867.8 ms (median, n=9) before the first encoder submit can start. The first
submit then runs at the warm speed (440.2 ms against 440.0 ms). The cost is
the open: the worker seals and hashes the 458 MB encoder program (434.8 ms on
the M1), and libane loads it into a DART-mapped buffer object (352.0 ms). A
`--repeat` run in one process pays the open once. Separate processes pay it
every time.

A long-lived worker that keeps the programs loaded removes the open from
every request after the first. Estimated gain on the T6001: about 868 ms per
CLI process (encoder stage 1313 ms to about 441 ms).

## Placement

The daemon is a third mode of the existing worker binary in omarchy-mlx:
`mlx-omarchy-ane-worker --listen`. The worker already owns the bundle
validation, the seal, the resident child, the bounded-submit protocol and
the inline payload format. A second implementation in omarchy-ane would
copy all of that. omarchy-ane keeps the device contract: the ioctls, libane,
and "a completed submit guarantees terminal completion and CPU-visible
outputs".

## Session identity

The daemon serves one fixed bundle set for its whole life. Its argv names the
bundles, the seal pins and the libane path, as `--serve` does today. At start
it seals and loads every program once.

The identity is the SHA-256 of the sorted lines `bundle file sha256`, the
libane SHA-256, the deadline and the iteration count. The client computes the
same value from its runtime pin. The daemon never loads a bundle that a
client names, so a client cannot make it load a program.

## Socket and permissions

- Path: `$XDG_RUNTIME_DIR/omarchy-ane/worker.sock`. The directory is 0700 and
  the socket is 0600. No TCP.
- The daemon checks `SO_PEERCRED` and refuses a peer with a different uid.
- The daemon runs as the user. The accel node is mode 0666, so it needs no
  privilege. One daemon per user; users do not share loaded programs.
- systemd user units: `omarchy-ane-worker.socket`
  (`ListenStream=%t/omarchy-ane/worker.sock`, `SocketMode=0600`,
  `DirectoryMode=0700`) and `omarchy-ane-worker.service`. Socket activation
  starts the daemon on the first connection. That connection pays the cold
  open once.

## Protocol

One connection is one client session. The daemon serves one connection at a
time; other clients wait in the listen backlog, bounded by their own
deadline. The driver `engine_lock` already serializes every submit on the
device.

```
client: hello 1 IDENTITY
daemon: ready PID PROGRAMS          (identity matches)
daemon: refused REASON              (identity differs, or quarantined)
then the existing resident lines, unchanged:
client: batch DEADLINE_MS | batch-end
client: submit NAME [--inline NAME=BYTES]... [--emit NAME]... + payload bytes
daemon: out NAME BYTES + bytes ... then "job status=0 ..." or "failed: ..."
client: bye                         (ends the connection; the daemon stays up)
```

`quit` is not accepted on the socket. Only the service manager stops the
daemon.

## Completion contract

The daemon sends `job status=0` only after `ane_exec` returned 0 and every
emitted output was copied out of its buffer object. `ANE_SUBMIT` returns only
after the driver saw the task complete (`ane_tm_execute`), and the default
`map_mode=3` mapping is coherent. So a `job status=0` line means terminal
completion and CPU-visible outputs, as today.

## Failures and restart

- A failed or late submit: the daemon sends `failed: ...` to the client and
  exits, as the resident session does today (it never retries). Socket
  activation starts a new daemon on the next connection, and that
  connection pays the cold open.
- The daemon dies or the connection drops after a submit was sent: the
  client reports the request as failed. It does not retry, because the
  device state of that submit is unknown.
- `connect` fails or the daemon answers `refused` before any submit: the
  client starts a private `--serve` worker, which is today's path.
- A wedged engine refuses every later submit (`-ECANCELED`). The daemon
  exits on the failed submit, and the close of its file runs the driver's
  per-file cleanup (`ane_drm_postclose`).

## Device and runtime PM

- The resident child keeps one accel file open while the daemon lives.
- origin/main holds a runtime-PM reference for the life of the device, so the
  daemon changes nothing there.
- With runtime-PM autosuspend, an open file does not hold the ANE awake: each
  ioctl takes and drops its own reference. The ANE can suspend between
  requests, and the first request after an idle gap pays one resume. The
  harness `gap` arm measures that resume.
- The daemon exits after `ANE_WORKER_IDLE_S` seconds without a connection
  (default 600). This frees its memory: about 1.4 GB for the encoder
  (INFERENCE: sealed memfd, libane staging copy and buffer object, 458 MB
  each).

## Seal contract change (owner decision)

Today every session seals and hashes every byte it consumes, and no process
trusts another process. The daemon seals once at start and then consumes
only its sealed memfds. A file that changes on disk after the start cannot
reach the device. A new install has a new pin, so its identity differs: the
daemon refuses, the client uses a private worker, and the old daemon exits
when idle or at `systemctl --user restart omarchy-ane-worker`. The change
from "authenticate per process" to "authenticate per daemon start" needs the
owner's approval before implementation.

## Cost

- Worker `--listen` mode, peer check, identity, idle exit: about 250 lines of
  C++ in omarchy-mlx.
- Client in `ane_resident.py`: connect, hello, fallback, about 80 lines.
- Two systemd user units and host tests for identity, refusal and fallback.

## Cheaper alternatives

The receipt ranks them. Short form: one DART TLB sync per buffer object in
`ane_iommu_map_pages` (driver), hash while loading through a libane
load-from-fd entry point, and no libane staging copy. Each removes part of
the 868 ms from every process; only the daemon removes all of it. A kernel
cache that keeps buffer objects after close is rejected: it breaks the
per-file teardown boundary and the wedge accounting.
