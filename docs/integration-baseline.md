# C++ standard-v1 integration baseline

Content revision: **v1-cpp-2026-10-05-r2** (frozen 
2026-10-05
).

| Project | Frozen HEAD |
|---|---|
| cpp-echo-client | 
6500e34a31660db03aeec3e90ef6eca8c618d154
 |
| cpp-echo-server | 
fd8e8e9440299f8a9f64a050cd4de49efda566f5
 |

The baseline equals the frozen content, so a baseline-relative patch is empty by definition:
this repository is the standard-v1 C++ implementation for its side. docs/integration-baseline.json
lists the SHA256 of every tracked file at the freeze, so any later edit is detectable.

## Verification at the freeze

- client Release 6/6 and Debug 6/6; server Release 5/5 and Debug 5/5, both including the 400-reset storm
- v1 conformance -Mode All: 104/104 (CLI 86, payload 8, interop 10)
- notify diagnostics gate: TCP 1/2/8/32 workers and UDP k=1/64/1024, per-worker gap in {0,1}, zero starvation wakeups
- 60 s soak: 4.2M echo/s, zero corrupted / lost / network errors, working set flat
- clean-room clone of the previous freeze built and passed every gate (see reports/baseline-v2-cleanroom.md in the workspace)

## Revision history

- v1-cpp-2026-10-05: client c67ede9, server 8a6a87b - v1 command line, metrics line and final statistics port
- v1-cpp-2026-10-05-r2: client 
6500e34a
, server 
fd8e8e94
 - RIONotify provider seam, opt-in diagnostics, hardened diagnostics gate

## What this record does not claim

- The older workspace baselines (client 10d1d7d, server 30de201) no longer describe the current C++; they are historical evidence only.
- Only the frozen commits above are covered; any later commit is a new revision that must be re-frozen.
