# staged-qwen: chained multi-program Qwen3.8-2B ANE runner (Linux)

- staged_qwen_manifest.py — macOS/ANEForge exporter: manifest.json + per-program build dirs + first-step goldens
- staged_qwen_runner.py — Linux runner (verify/replay/bench) over any backend-module
- libane_model.py — libane_python.so backend (multi-surface ANEC)
- inproc_backend.py — e5rt-bundle inproc-shim backend (needs per-bundle manifest.json from the shim tooling)
- run-e5rt-backend.py + staged_e5rt_backend.py — macOS validation rig (runner logic vs live e5rt programs)

Receipt: joshuawarren/ane-linux-experiments receipts/2026-09-25-qwen-ane-decoder-fix/
