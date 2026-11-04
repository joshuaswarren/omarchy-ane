# Correction

See `../2026-11-04-j700-h17-boot-safety-review/README.md`.
The observations in probe.log stand, but a latched RVBAR does not establish
firmware contents or working DART mappings. Its decoded vector is
`0x10000000000`, not `0x1000000000`. A negative wake test does not isolate
the clock as the cause. The generic H16 boot path is not yet safe for H17.
