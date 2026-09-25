# staged-qwen: chained multi-program Qwen3.8-2B ANE runner (Linux)

- staged_qwen_manifest.py — macOS/ANEForge exporter: manifest.json + per-program build dirs + first-step goldens
- io_layout.py — per-surface I/O layout from the compiled HWX (channel table + STABS strides): `plan` measures each surface's ANEC channel, packed geometry and tile count; `apply` writes them into the ANEC headers and the role -> channel bind (src_channels/dst_channels) into manifest.json
- staged_qwen_runner.py — Linux runner (verify/replay/bench) over any backend-module
- libane_model.py — libane_python.so backend: bind_load (pyane_bind_load) + ane_tile/ane_untile packing at each channel's header geometry
- inproc_backend.py — e5rt-bundle inproc-shim backend (needs per-bundle manifest.json from the shim tooling)
- run-e5rt-backend.py + staged_e5rt_backend.py — macOS validation rig (runner logic vs live e5rt programs)
- e5rt_port_layout.py — macOS: the e5rt io-port tensor descriptors per program (dense logical view)
- ane_request_capture.m + capture_step_surfaces.py — macOS: dense port values and the IOSurfaces the ANE runtime hands the engine, for decode step 0 of p001

Receipt: joshuawarren/ane-linux-experiments receipts/2026-09-25-qwen-ane-decoder-fix/
